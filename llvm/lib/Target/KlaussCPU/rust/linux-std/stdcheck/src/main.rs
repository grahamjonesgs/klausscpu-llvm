//! std on KlaussCPU Linux: one check per area of std that talks to the OS.
use std::collections::HashMap;
use std::io::{Read, Write};
use std::sync::{mpsc, Arc, Mutex};
use std::time::{Duration, Instant};

static mut FAILS: u32 = 0;
fn check(name: &str, ok: bool) {
    println!("{} {}", if ok { "PASS" } else { "FAIL" }, name);
    if !ok {
        unsafe { FAILS += 1 }
    }
}

thread_local! {
    static TL: std::cell::Cell<u64> = const { std::cell::Cell::new(7) };
}

fn main() {
    println!("hello from Rust std on {} {}", std::env::consts::ARCH, std::env::consts::OS);
    let args: Vec<String> = std::env::args().collect();
    check("env::args", !args.is_empty());
    check("env::var PATH", std::env::var("PATH").is_ok());

    let mut m = HashMap::new();
    for i in 0..1000u64 {
        m.insert(i, i * i);
    }
    check("HashMap (RandomState from getrandom)", m.len() == 1000 && m[&999] == 998001);
    let s: String = (0..100).map(|i| format!("{i:x}")).collect();
    check("format/String", s.len() == 184);
    check("f64 formatting", format!("{:.3}", 2.0f64.sqrt()) == "1.414");
    check("u128 arithmetic", (u128::MAX / 3).to_string() == "113427455640312821154458202477256070485");

    // Threads, Mutex, Arc, channels, thread_local.
    let counter = Arc::new(Mutex::new(0u64));
    let (tx, rx) = mpsc::channel();
    let hs: Vec<_> = (0..4)
        .map(|t| {
            let c = Arc::clone(&counter);
            let tx = tx.clone();
            std::thread::spawn(move || {
                TL.with(|v| v.set(t));
                for _ in 0..10000 {
                    *c.lock().unwrap() += 1;
                }
                tx.send(TL.with(|v| v.get())).unwrap();
            })
        })
        .collect();
    for h in hs {
        h.join().unwrap();
    }
    drop(tx);
    let mut got: Vec<u64> = rx.iter().collect();
    got.sort();
    check("threads + Mutex", *counter.lock().unwrap() == 40000);
    check("channels + thread_local", got == vec![0, 1, 2, 3] && TL.with(|v| v.get()) == 7);
    let a = Arc::new(std::sync::atomic::AtomicU64::new(0));
    let hs: Vec<_> = (0..4)
        .map(|_| {
            let a = Arc::clone(&a);
            std::thread::spawn(move || {
                for _ in 0..10000 {
                    a.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
                }
            })
        })
        .collect();
    hs.into_iter().for_each(|h| h.join().unwrap());
    check("AtomicU64 across threads", a.load(std::sync::atomic::Ordering::SeqCst) == 40000);

    // Files.
    let dir = std::env::temp_dir().join(format!("stdcheck-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let f = dir.join("a.txt");
    std::fs::write(&f, b"klausscpu\n").unwrap();
    let md = std::fs::metadata(&f).unwrap();
    check("fs write/metadata", md.len() == 10 && md.is_file());
    check("fs read", std::fs::read_to_string(&f).unwrap() == "klausscpu\n");
    std::fs::rename(&f, dir.join("b.txt")).unwrap();
    let names: Vec<_> = std::fs::read_dir(&dir).unwrap().map(|e| e.unwrap().file_name()).collect();
    check("fs rename/read_dir", names.len() == 1 && names[0] == "b.txt");
    {
        use std::os::unix::fs::MetadataExt;
        let md = std::fs::metadata(dir.join("b.txt")).unwrap();
        check("stat fields (mode, nlink, uid, mtime)", md.mode() & 0o170000 == 0o100000 && md.nlink() == 1 && md.mtime() > 1_700_000_000);
    }
    std::fs::remove_dir_all(&dir).unwrap();
    check("fs remove_dir_all", !dir.exists());

    // Processes.
    let out = std::process::Command::new("/bin/echo").arg("child").output().unwrap();
    check("process::Command output", out.status.success() && out.stdout == b"child\n");
    let st = std::process::Command::new("/bin/sh").args(["-c", "exit 3"]).status().unwrap();
    check("exit status", st.code() == Some(3));
    let mut child = std::process::Command::new("/bin/cat")
        .stdin(std::process::Stdio::piped())
        .stdout(std::process::Stdio::piped())
        .spawn()
        .unwrap();
    child.stdin.take().unwrap().write_all(b"piped").unwrap();
    let mut buf = String::new();
    child.stdout.take().unwrap().read_to_string(&mut buf).unwrap();
    check("pipes to a child", child.wait().unwrap().success() && buf == "piped");

    // Time.
    let t0 = Instant::now();
    std::thread::sleep(Duration::from_millis(50));
    let dt = t0.elapsed();
    check("sleep/Instant", dt >= Duration::from_millis(50) && dt < Duration::from_secs(2));
    let now = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap();
    check("SystemTime", now.as_secs() > 1_700_000_000);

    // Networking on loopback.
    let l = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
    let addr = l.local_addr().unwrap();
    let srv = std::thread::spawn(move || {
        let (mut s, _) = l.accept().unwrap();
        let mut b = [0u8; 4];
        s.read_exact(&mut b).unwrap();
        s.write_all(&b.map(|c| c.to_ascii_uppercase())).unwrap();
    });
    let mut c = std::net::TcpStream::connect(addr).unwrap();
    c.write_all(b"ping").unwrap();
    let mut b = [0u8; 4];
    c.read_exact(&mut b).unwrap();
    srv.join().unwrap();
    check("TCP loopback", &b == b"PING");
    let u = std::net::UdpSocket::bind("127.0.0.1:0").unwrap();
    u.send_to(b"udp", u.local_addr().unwrap()).unwrap();
    let mut ub = [0u8; 8];
    let (n, _) = u.recv_from(&mut ub).unwrap();
    check("UDP loopback", &ub[..n] == b"udp");
    let resolved = std::net::ToSocketAddrs::to_socket_addrs("localhost:80").map(|a| a.count()).unwrap_or(0);
    check("getaddrinfo localhost", resolved > 0);

    // Panics: unwinding through frames, Drop runs, catch_unwind.
    struct D<'a>(&'a mut bool);
    impl Drop for D<'_> {
        fn drop(&mut self) {
            *self.0 = true;
        }
    }
    let mut dropped = false;
    std::panic::set_hook(Box::new(|_| {}));
    let r = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        let _d = D(&mut dropped);
        let v: Vec<u32> = Vec::new();
        #[allow(unconditional_panic)]
        let x = v[std::hint::black_box(3)];
        x
    }));
    let _ = std::panic::take_hook();
    check("panic unwinds, runs Drop, catch_unwind", r.is_err() && dropped);
    let j = std::thread::spawn(|| panic!("in a thread")).join();
    check("panic in a thread -> join Err", j.is_err());
    check("backtrace capture", { let _ = std::backtrace::Backtrace::force_capture().to_string(); true });

    let f = unsafe { FAILS };
    println!("stdcheck: {}", if f == 0 { "PASS".to_string() } else { format!("{f} FAIL") });
    std::process::exit(if f == 0 { 0 } else { 1 });
}
