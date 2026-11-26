using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Net;
using System.Text;
using System.Web.Script.Serialization;

namespace CS2ServerControl
{
    internal static class Json
    {
        private static readonly JavaScriptSerializer Ser = new JavaScriptSerializer { MaxJsonLength = int.MaxValue };

        public static Dictionary<string, object> ParseObject(string json)
        {
            var o = Ser.DeserializeObject(json) as Dictionary<string, object>;
            return o ?? new Dictionary<string, object>();
        }

        public static object Parse(string json) { return Ser.DeserializeObject(json); }

        public static Dictionary<string, object> LoadObject(string path)
        {
            try
            {
                if (File.Exists(path)) return ParseObject(StripComments(File.ReadAllText(path)));
            }
            catch { }
            return new Dictionary<string, object>();
        }

        public static void Save(string path, object value)
        {
            Directory.CreateDirectory(Path.GetDirectoryName(path));
            var tmp = path + ".tmp";
            File.WriteAllText(tmp, Pretty(value, 0), new UTF8Encoding(false));
            if (File.Exists(path)) File.Delete(path);
            File.Move(tmp, path);
        }

        private static string StripComments(string s)
        {
            var sb = new StringBuilder();
            bool inStr = false;
            for (int i = 0; i < s.Length; i++)
            {
                char c = s[i];
                if (inStr)
                {
                    sb.Append(c);
                    if (c == '\\' && i + 1 < s.Length) { sb.Append(s[++i]); continue; }
                    if (c == '"') inStr = false;
                    continue;
                }
                if (c == '"') { inStr = true; sb.Append(c); continue; }
                if (c == '/' && i + 1 < s.Length && s[i + 1] == '/')
                {
                    while (i < s.Length && s[i] != '\n') i++;
                    continue;
                }
                sb.Append(c);
            }
            return sb.ToString();
        }

        public static string Pretty(object v, int indent)
        {
            var pad = new string(' ', indent * 2);
            var pad2 = new string(' ', (indent + 1) * 2);
            if (v == null) return "null";
            if (v is string) return Quote((string)v);
            if (v is bool) return ((bool)v) ? "true" : "false";
            if (v is int || v is long || v is ulong || v is uint || v is short) return Convert.ToString(v, CultureInfo.InvariantCulture);
            if (v is double || v is float || v is decimal) return Convert.ToDouble(v, CultureInfo.InvariantCulture).ToString("R", CultureInfo.InvariantCulture);
            var dict = v as IDictionary;
            if (dict != null)
            {
                if (dict.Count == 0) return "{}";
                var parts = new List<string>();
                foreach (DictionaryEntry e in dict) parts.Add(pad2 + Quote(e.Key.ToString()) + ": " + Pretty(e.Value, indent + 1));
                return "{\n" + string.Join(",\n", parts.ToArray()) + "\n" + pad + "}";
            }
            var list = v as IEnumerable;
            if (list != null)
            {
                var parts = new List<string>();
                bool simple = true;
                foreach (var x in list)
                {
                    if (x is IDictionary || (x is IEnumerable && !(x is string))) simple = false;
                    parts.Add(Pretty(x, indent + 1));
                }
                if (parts.Count == 0) return "[]";
                if (simple) return "[" + string.Join(", ", parts.ToArray()) + "]";
                var lines = new List<string>();
                foreach (var p in parts) lines.Add(pad2 + p);
                return "[\n" + string.Join(",\n", lines.ToArray()) + "\n" + pad + "]";
            }
            return Quote(v.ToString());
        }

        public static string Quote(string s)
        {
            var sb = new StringBuilder("\"");
            foreach (var c in s)
            {
                switch (c)
                {
                    case '"': sb.Append("\\\""); break;
                    case '\\': sb.Append("\\\\"); break;
                    case '\n': sb.Append("\\n"); break;
                    case '\r': sb.Append("\\r"); break;
                    case '\t': sb.Append("\\t"); break;
                    default:
                        if (c < ' ') sb.AppendFormat("\\u{0:x4}", (int)c); else sb.Append(c);
                        break;
                }
            }
            return sb.Append('"').ToString();
        }

        public static string Str(Dictionary<string, object> d, string key, string def = "")
        {
            object v;
            return d != null && d.TryGetValue(key, out v) && v != null ? Convert.ToString(v, CultureInfo.InvariantCulture) : def;
        }

        public static bool Bool(Dictionary<string, object> d, string key, bool def)
        {
            object v;
            if (d == null || !d.TryGetValue(key, out v) || v == null) return def;
            if (v is bool) return (bool)v;
            bool b;
            return bool.TryParse(v.ToString(), out b) ? b : def;
        }

        public static int Int(Dictionary<string, object> d, string key, int def)
        {
            object v;
            if (d == null || !d.TryGetValue(key, out v) || v == null) return def;
            int i;
            return int.TryParse(Convert.ToString(v, CultureInfo.InvariantCulture), out i) ? i : def;
        }
    }

    internal static class Http
    {
        static Http()
        {
            // Windows 7/8 with .NET 4.x don't always enable TLS 1.2 by themselves.
            ServicePointManager.SecurityProtocol |= (SecurityProtocolType)3072;
        }

        public static WebClient Client()
        {
            var wc = new WebClient();
            wc.Headers[HttpRequestHeader.UserAgent] = "CS2-Server-Control/1.0 (+https://github.com/Casio-Byte)";
            wc.Encoding = Encoding.UTF8;
            return wc;
        }

        public static string Get(string url)
        {
            using (var wc = Client()) return wc.DownloadString(url);
        }

        public static void Download(string url, string file, Action<int> progress)
        {
            Directory.CreateDirectory(Path.GetDirectoryName(file));
            var req = (HttpWebRequest)WebRequest.Create(url);
            req.UserAgent = "CS2-Server-Control/1.0";
            req.AllowAutoRedirect = true;
            req.Timeout = 60000;
            req.ReadWriteTimeout = 60000;
            using (var resp = (HttpWebResponse)req.GetResponse())
            using (var s = resp.GetResponseStream())
            using (var f = File.Create(file + ".part"))
            {
                long total = resp.ContentLength, done = 0;
                var buf = new byte[1 << 16];
                int n, last = -1;
                while ((n = s.Read(buf, 0, buf.Length)) > 0)
                {
                    f.Write(buf, 0, n);
                    done += n;
                    if (total > 0 && progress != null)
                    {
                        int pct = (int)(done * 100 / total);
                        if (pct != last) { last = pct; progress(pct); }
                    }
                }
            }
            if (File.Exists(file)) File.Delete(file);
            File.Move(file + ".part", file);
        }

        public static string Post(string url, string form)
        {
            using (var wc = Client())
            {
                wc.Headers[HttpRequestHeader.ContentType] = "application/x-www-form-urlencoded";
                return wc.UploadString(url, form);
            }
        }
    }

    internal static class Rand
    {
        public static string Password(int len)
        {
            const string chars = "abcdefghjkmnpqrstuvwxyzABCDEFGHJKMNPQRSTUVWXYZ23456789";
            var bytes = new byte[len];
            using (var rng = System.Security.Cryptography.RandomNumberGenerator.Create()) rng.GetBytes(bytes);
            var sb = new StringBuilder();
            foreach (var b in bytes) sb.Append(chars[b % chars.Length]);
            return sb.ToString();
        }
    }
}
