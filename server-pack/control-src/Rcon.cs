using System;
using System.IO;
using System.Net.Sockets;
using System.Text;

namespace CS2ServerControl
{
    /// Minimal Source RCON client (TCP). CS2 needs -usercon on the command line for this to work.
    internal sealed class Rcon : IDisposable
    {
        private const int SERVERDATA_AUTH = 3, SERVERDATA_EXECCOMMAND = 2, SERVERDATA_RESPONSE_VALUE = 0, SERVERDATA_AUTH_RESPONSE = 2;
        private TcpClient _tcp;
        private NetworkStream _s;
        private int _id = 10;

        public static string Run(string host, int port, string password, string command, int timeoutMs = 3000)
        {
            using (var r = new Rcon())
            {
                r.Connect(host, port, password, timeoutMs);
                return r.Exec(command);
            }
        }

        public void Connect(string host, int port, string password, int timeoutMs)
        {
            _tcp = new TcpClient();
            var ar = _tcp.BeginConnect(host, port, null, null);
            if (!ar.AsyncWaitHandle.WaitOne(timeoutMs)) throw new IOException("RCON: no answer from " + host + ":" + port);
            _tcp.EndConnect(ar);
            _tcp.ReceiveTimeout = timeoutMs;
            _tcp.SendTimeout = timeoutMs;
            _s = _tcp.GetStream();

            int authId = ++_id;
            Send(authId, SERVERDATA_AUTH, password);
            // The server answers with an empty RESPONSE_VALUE and then AUTH_RESPONSE (id -1 = wrong password).
            for (int i = 0; i < 4; i++)
            {
                int id, type; string body;
                Read(out id, out type, out body);
                if (type == SERVERDATA_AUTH_RESPONSE)
                {
                    if (id == -1) throw new UnauthorizedAccessException("RCON: wrong password");
                    return;
                }
            }
            throw new IOException("RCON: no auth response");
        }

        public string Exec(string command)
        {
            int cmdId = ++_id, endId = ++_id;
            Send(cmdId, SERVERDATA_EXECCOMMAND, command);
            // An empty RESPONSE_VALUE after the command marks the end of a multi-packet answer.
            Send(endId, SERVERDATA_RESPONSE_VALUE, "");
            // CS2 does not always answer the empty end-marker packet, and may tag replies with another id,
            // so every RESPONSE_VALUE body counts until the marker comes back or the server goes quiet.
            var sb = new StringBuilder();
            var deadline = DateTime.UtcNow.AddSeconds(6);
            _tcp.ReceiveTimeout = 1500;
            while (DateTime.UtcNow < deadline)
            {
                int id, type; string body;
                try { Read(out id, out type, out body); }
                catch (IOException) { break; }
                if (id == endId && body.Length == 0) break;
                if (type == SERVERDATA_RESPONSE_VALUE) sb.Append(body);
            }
            return sb.ToString();
        }

        private void Send(int id, int type, string body)
        {
            var b = Encoding.UTF8.GetBytes(body);
            var pkt = new byte[14 + b.Length];
            BitConverter.GetBytes(10 + b.Length).CopyTo(pkt, 0);
            BitConverter.GetBytes(id).CopyTo(pkt, 4);
            BitConverter.GetBytes(type).CopyTo(pkt, 8);
            b.CopyTo(pkt, 12);
            _s.Write(pkt, 0, pkt.Length);
        }

        private void Read(out int id, out int type, out string body)
        {
            var len = BitConverter.ToInt32(ReadN(4), 0);
            if (len < 10 || len > 1 << 20) throw new IOException("RCON: bad packet");
            var data = ReadN(len);
            id = BitConverter.ToInt32(data, 0);
            type = BitConverter.ToInt32(data, 4);
            body = Encoding.UTF8.GetString(data, 8, Math.Max(0, len - 10));
        }

        private byte[] ReadN(int n)
        {
            var buf = new byte[n];
            int got = 0;
            while (got < n)
            {
                int r = _s.Read(buf, got, n - got);
                if (r <= 0) throw new IOException("RCON: connection closed");
                got += r;
            }
            return buf;
        }

        public void Dispose()
        {
            try { if (_s != null) _s.Dispose(); } catch { }
            try { if (_tcp != null) _tcp.Close(); } catch { }
        }
    }
}
