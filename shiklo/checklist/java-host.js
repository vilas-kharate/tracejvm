// TraceJVM system host for the Java editor (PoC, to become the tool's java-host.js): the project as an in-memory POSIX
// file system under /home/student, the program's standard descriptors, and sockets on localhost. TraceJVM calls dispatch({service:"posix",
// operation, payload}) from its VM worker (synchronously through shared memory, or asynchronously for blocking reads);
// shapes follow its browser-worker.js (tracejvm_host_*_js). Errors are Error objects whose name is the errno (ENOENT …).
//   const host = createHost({ cwd, files: {path: Uint8Array}, onOut(text), onErr(text), readLine(): Promise<string|null> })
//   host.files() → {path: Uint8Array} (relative to cwd)   host.dispatch(request)
const S_IFREG = 0o100000, S_IFDIR = 0o040000;

// onExit(status): the program called System.exit (the launcher writes ESC ] lmpb-exit;<status> BEL to fd 2); that
// write never returns, so no thread runs on while the page stops the VM.
const EXIT_MARK = /\u001b\]lmpb-exit;(-?\d+)\u0007/;

export function createHost({ cwd = "/home/student", files = {}, onOut = () => {}, onErr = () => {}, onExit = () => {}, readLine = async () => null, log = null } = {}) {
  const dec1 = new TextDecoder(), dec2 = new TextDecoder();
  const enc = new TextEncoder();
  const nodes = new Map();            // absolute path → { dir: true } | { data: Uint8Array, mtime }
  let inode = 1;
  const mkdirs = (p) => {
    let cur = "";
    for (const x of ["", ...p.split("/").filter(Boolean)]) {
      cur = !x ? "/" : cur === "/" ? `/${x}` : `${cur}/${x}`;
      if (!nodes.has(cur)) nodes.set(cur, { dir: true, ino: inode++, mtime: Date.now() });
    }
  };
  mkdirs(cwd);
  mkdirs("/tmp");
  const norm = (path) => {
    const abs = path.startsWith("/") ? path : `${cwd}/${path}`;
    const out = [];
    for (const part of abs.split("/")) { if (!part || part === ".") continue; if (part === "..") out.pop(); else out.push(part); }
    return "/" + out.join("/");
  };
  const parent = (p) => p.slice(0, p.lastIndexOf("/")) || "/";
  for (const [rel, data] of Object.entries(files)) { const p = norm(rel); if (rel.endsWith("/")) mkdirs(p); else { mkdirs(parent(p)); nodes.set(p, { data, ino: inode++, mtime: Date.now() }); } }
  const err = (name, message = name) => Object.assign(new Error(message), { name });
  const fds = new Map();              // fd → { path, pos, append, read, write }
  let nextFd = 3;
  let stdinBuf = new Uint8Array(0), stdinEof = false;
  const statOf = (p) => {
    const n = nodes.get(p);
    if (!n) throw err("ENOENT", `No such file or directory: ${p}`);
    const t = n.mtime || 0;
    return { stat: { kind: n.dir ? "directory" : "file", mode: n.dir ? S_IFDIR | 0o755 : S_IFREG | 0o644, inode: n.ino, nlink: 1, size: n.dir ? 0 : n.data.length, createdAt: t, modifiedAt: t, changedAt: t } };
  };
  const take = (max) => { const b = stdinBuf.subarray(0, max); stdinBuf = stdinBuf.slice(b.length); return b.slice(); };
  async function readStdin(max) {
    if (stdinBuf.length) return take(max);
    if (stdinEof) return new Uint8Array(0);
    const line = await readLine();
    if (line == null) { stdinEof = true; return new Uint8Array(0); }
    stdinBuf = enc.encode(line.endsWith("\n") ? line : `${line}\n`);
    return take(max);
  }

  // ---- Sockets on localhost: the network a program has in the browser (sockets between its own threads). A connect
  // to a listening port makes a pair of connected sockets; reads and accepts that would block return a Promise, which
  // TraceJVM waits for while the program's other threads run. Any address but 127.0.0.1 / localhost is unreachable,
  // as on a computer without a network.
  const sockets = new Map();          // fd → socket
  const listeners = new Map();        // port → listening socket
  let nextPort = 49152;
  const LOOPBACK = new Set(["127.0.0.1", "localhost", "0.0.0.0", "::1", "::", ""]);
  const sock = (fd) => { const s = sockets.get(fd); if (!s) throw err("EBADF"); return s; };
  const freePort = () => { while (listeners.has(nextPort) || [...sockets.values()].some((s) => s.port === nextPort)) nextPort++; return nextPort++; };
  const newSocket = () => { const s = { fd: nextFd++, port: 0, peer: null, chunks: [], eof: false, listening: false, backlog: [], waiters: [], nonblocking: false, closed: false }; sockets.set(s.fd, s); return s; };
  const wake = (s) => { const w = s.waiters; s.waiters = []; for (const f of w) f(); };
  const readable = (s) => s.chunks.length > 0 || s.eof || (s.listening && s.backlog.length > 0);
  // The bytes waiting on s, at most max (an empty array is the end of the stream).
  const takeBytes = (s, max) => {
    const out = []; let n = 0;
    while (s.chunks.length && n < max) { const c = s.chunks[0], k = Math.min(c.length, max - n); out.push(c.subarray(0, k)); n += k; if (k === c.length) s.chunks.shift(); else s.chunks[0] = c.subarray(k); }
    const bytes = new Uint8Array(n); let at = 0; for (const c of out) { bytes.set(c, at); at += c.length; }
    return bytes;
  };
  const whenReadable = (s, answer) => (readable(s) ? answer() : s.nonblocking ? (() => { throw err("EAGAIN"); })()
    : new Promise((resolve, reject) => s.waiters.push(() => { try { resolve(answer()); } catch (e) { reject(e); } })));
  const accepted = (s) => {
    if (s.closed || !s.backlog.length) throw err("EBADF", "Socket closed");
    const c = s.backlog.shift();
    return { fd: c.fd, remoteAddress: { host: "127.0.0.1", port: c.peer.port } };
  };
  function closeSocket(s) {
    s.closed = true; sockets.delete(s.fd);
    if (s.listening && listeners.get(s.port) === s) listeners.delete(s.port);
    if (s.peer) { s.peer.eof = true; wake(s.peer); }
    s.eof = true; wake(s);
  }
  function socketCall(op, p) {
    switch (op) {
      case "socket": return { fd: newSocket().fd };
      case "bind": {
        const s = sock(p.fd), a = p.address || {};
        if (!LOOPBACK.has(a.host ?? "")) throw err("EADDRNOTAVAIL", "Cannot assign requested address");
        if (a.port && (listeners.has(a.port) || [...sockets.values()].some((o) => o !== s && o.port === a.port))) throw err("EADDRINUSE", "Address already in use");
        s.port = a.port || freePort();
        return { address: { host: "127.0.0.1", port: s.port } };
      }
      case "listen": { const s = sock(p.fd); if (!s.port) s.port = freePort(); s.listening = true; listeners.set(s.port, s); return {}; }
      case "connect": {
        const s = sock(p.fd), a = p.address || {};
        if (!LOOPBACK.has(a.host ?? "")) throw err("ENETUNREACH", "Network is unreachable");
        const l = listeners.get(a.port);
        if (!l) throw err("ECONNREFUSED", "Connection refused");
        if (!s.port) s.port = freePort();
        const end = newSocket(); end.port = l.port; end.peer = s; s.peer = end;
        l.backlog.push(end); wake(l);
        return {};
      }
      case "accept": { const s = sock(p.fd); return whenReadable(s, () => accepted(s)); }
      case "read": { const s = sock(p.fd); return whenReadable(s, () => ({ bytes: takeBytes(s, p.maxBytes) })); }
      case "write": {
        const s = sock(p.fd);
        if (!s.peer || s.peer.closed || s.peer.eof) throw err("EPIPE", "Broken pipe");
        s.peer.chunks.push(p.bytes.slice()); wake(s.peer);
        return { bytesWritten: p.bytes.length };
      }
      case "shutdown": {
        const s = sock(p.fd);
        if (p.how !== "read" && s.peer) { s.peer.eof = true; wake(s.peer); }
        if (p.how !== "write") { s.eof = true; wake(s); }
        return {};
      }
      case "close": closeSocket(sock(p.fd)); return {};
      case "getsockname": return { address: { host: "127.0.0.1", port: sock(p.fd).port } };
      case "getpeername": { const s = sock(p.fd); if (!s.peer) throw err("ENOTCONN"); return { address: { host: "127.0.0.1", port: s.peer.port } }; }
      case "fcntl": { const s = sock(p.fd); if (p.action === "set-nonblocking") s.nonblocking = !!p.nonblocking; return {}; }
      case "fstat": return { stat: { kind: "socket", mode: 0o140777, inode: 0, nlink: 1, size: 0, createdAt: 0, modifiedAt: 0, changedAt: 0 } };
    }
    return undefined;
  }

  function dispatch(req) {
    const p = req.payload || {};
    if (log) log(req);
    if (req.service !== "posix") throw err("ENOSYS", `${req.service}.${req.operation} is not available here`);
    if (req.operation === "socket" || (p.fd !== undefined && sockets.has(p.fd))) {
      const r = socketCall(req.operation, p);
      if (r !== undefined) return r;
    }
    switch (req.operation) {
      case "fcntl": return {};          // files and the standard descriptors: blocking mode only
      case "bind": case "listen": case "connect": case "accept": case "getsockname": case "getpeername": case "shutdown":
        throw err("ENOTSOCK", "Socket operation on non-socket");
      case "read": {
        if (p.fd === 0) return readStdin(p.maxBytes).then((bytes) => ({ bytes }));
        const f = fds.get(p.fd); if (!f) throw err("EBADF");
        const n = nodes.get(f.path); if (!n || n.dir) throw err("EISDIR");
        const at = p.position ?? f.pos, bytes = n.data.slice(at, at + p.maxBytes);
        if (p.position === undefined) f.pos += bytes.length;
        return { bytes };
      }
      case "write": {
        if (p.fd === 1) { onOut(dec1.decode(p.bytes, { stream: true })); return { bytesWritten: p.bytes.length }; }
        if (p.fd === 2) {
          const text = dec2.decode(p.bytes, { stream: true }), m = EXIT_MARK.exec(text);
          if (m) { if (m.index) onErr(text.slice(0, m.index)); onExit(+m[1]); return new Promise(() => {}); }
          onErr(text); return { bytesWritten: p.bytes.length };
        }
        const f = fds.get(p.fd); if (!f) throw err("EBADF");
        const n = nodes.get(f.path);
        const at = f.append ? n.data.length : (p.position ?? f.pos);
        const data = new Uint8Array(Math.max(n.data.length, at + p.bytes.length)); data.set(n.data); data.set(p.bytes, at);
        n.data = data; n.mtime = Date.now();
        if (p.position === undefined) f.pos = at + p.bytes.length;
        return { bytesWritten: p.bytes.length };
      }
      case "open": {
        const path = norm(p.path), o = p.options || {};
        let n = nodes.get(path);
        if (n && o.create && o.exclusive) throw err("EEXIST");
        if (!n) {
          if (!o.create) throw err("ENOENT", `No such file or directory: ${p.path}`);
          if (!nodes.get(parent(path))?.dir) throw err("ENOENT");
          n = { data: new Uint8Array(0), ino: inode++, mtime: Date.now() }; nodes.set(path, n);
        }
        if (n.dir && o.access !== "read") throw err("EISDIR");
        if (o.truncate && !n.dir) { n.data = new Uint8Array(0); n.mtime = Date.now(); }
        const fd = nextFd++; fds.set(fd, { path, pos: 0, append: !!o.append }); return { fd };
      }
      case "close": if (p.fd > 2) fds.delete(p.fd); return {};
      case "seek": {
        const f = fds.get(p.fd); if (!f) throw err(p.fd <= 2 ? "ESPIPE" : "EBADF");
        const size = nodes.get(f.path).data?.length || 0;
        f.pos = p.whence === "set" ? p.offset : p.whence === "current" ? f.pos + p.offset : size + p.offset;
        return { offset: f.pos };
      }
      case "stat": case "lstat": return statOf(norm(p.path));
      case "fstat": {
        if (p.fd <= 2) return { stat: { kind: "file", mode: 0o020620, inode: 0, nlink: 1, size: 0, createdAt: 0, modifiedAt: 0, changedAt: 0 } };
        const f = fds.get(p.fd); if (!f) throw err("EBADF"); return statOf(f.path);
      }
      case "ftruncate": { const f = fds.get(p.fd); const n = f && nodes.get(f.path); if (!n) throw err("EBADF"); n.data = n.data.slice(0, p.length); return {}; }
      case "poll":            // standard input is "ready" when typed text is waiting; files always are; sockets when
        return { entries: (p.entries || []).map((e) => {   // bytes, a connection or the end of the stream wait
          const s = sockets.get(e.fd);
          if (s) return { fd: e.fd, read: e.read && readable(s), write: e.write && !!s.peer && !s.peer.closed, hangup: s.eof && !s.chunks.length };
          return { fd: e.fd, read: e.read && (e.fd !== 0 || stdinBuf.length > 0), write: !!e.write };
        }) };
      case "mkdir": { const path = norm(p.path); if (nodes.has(path)) throw err("EEXIST"); if (!nodes.get(parent(path))?.dir) throw err("ENOENT"); nodes.set(path, { dir: true, ino: inode++, mtime: Date.now() }); return {}; }
      case "rmdir": { const path = norm(p.path); const n = nodes.get(path); if (!n) throw err("ENOENT"); if (!n.dir) throw err("ENOTDIR");
        if ([...nodes.keys()].some((k) => k.startsWith(path + "/"))) throw err("ENOTEMPTY"); nodes.delete(path); return {}; }
      case "unlink": { const path = norm(p.path); const n = nodes.get(path); if (!n) throw err("ENOENT"); if (n.dir) throw err("EISDIR"); nodes.delete(path); return {}; }
      case "rename": {
        const a = norm(p.sourcePath), b = norm(p.destinationPath);
        if (!nodes.has(a)) throw err("ENOENT");
        for (const k of [...nodes.keys()]) if (k === a || k.startsWith(a + "/")) { const v = nodes.get(k); nodes.delete(k); nodes.set(b + k.slice(a.length), v); }
        return {};
      }
      case "readdir": {
        const path = norm(p.path); if (!nodes.get(path)?.dir) throw err("ENOTDIR");
        const pre = path === "/" ? "/" : path + "/";
        return { entries: [...nodes.keys()].filter((k) => k.startsWith(pre) && k.length > pre.length && !k.slice(pre.length).includes("/")).map((k) => ({ name: k.slice(pre.length) })) };
      }
      case "realpath": { const path = norm(p.path); if (!nodes.has(path)) throw err("ENOENT"); return { path }; }
      case "access": { statOf(norm(p.path)); return {}; }
      case "environment": return { entries: [] };
      default: throw err("ENOSYS", `posix.${req.operation} is not available in the browser`);
    }
  }

  return {
    dispatch,
    /** The project after the run: files and folders under cwd (folders end with "/"). */
    files() {
      const out = {}, pre = cwd + "/";
      for (const [k, n] of nodes) if (k.startsWith(pre)) out[k.slice(pre.length) + (n.dir ? "/" : "")] = n.dir ? new Uint8Array(0) : n.data;
      return out;
    },
  };
}
