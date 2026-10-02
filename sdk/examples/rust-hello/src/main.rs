//! A Rust program running natively on QRT: Rust's standard library on the QRT SDK's musl
//! (QRT's own system calls).  Threads, channels, a mutex, files, time, hashing (random
//! seeds) and a panic-free exit.  Prints "rust: ok".
use std::collections::HashMap;
use std::sync::{mpsc, Arc, Mutex};
use std::time::{Duration, Instant};
use std::{fs, thread};

fn main() {
    let mut ok = true;
    let mut check = |cond: bool, what: &str| {
        println!("rust: {} {}", what, if cond { "ok" } else { "FAILED" });
        ok &= cond;
    };

    let counter = Arc::new(Mutex::new(0u32));
    let (tx, rx) = mpsc::channel();
    let handles: Vec<_> = (0..4)
        .map(|i| {
            let (c, tx) = (Arc::clone(&counter), tx.clone());
            thread::spawn(move || {
                for _ in 0..1000 { *c.lock().unwrap() += 1; }
                tx.send(i).unwrap();
            })
        })
        .collect();
    for h in handles { h.join().unwrap(); }
    drop(tx);
    let got: u32 = rx.iter().sum();
    check(*counter.lock().unwrap() == 4000 && got == 6, "threads, mutex, channel");

    fs::write("/tmp/rust.txt", "from Rust").unwrap();
    let back = fs::read_to_string("/tmp/rust.txt").unwrap_or_default();
    check(back == "from Rust" && fs::remove_file("/tmp/rust.txt").is_ok(), "files");

    let t = Instant::now();
    thread::sleep(Duration::from_millis(20));
    check(t.elapsed() >= Duration::from_millis(19), "time");

    let mut m = HashMap::new();
    for i in 0..1000 { m.insert(i, i * i); }
    check(m[&999] == 998001, "HashMap (random seeds)");

    println!("rust: {}", if ok { "ok" } else { "FAILED" });
    std::process::exit(if ok { 0 } else { 1 });
}
