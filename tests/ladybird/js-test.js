// js-test.js - Ladybird's JavaScript engine (LibJS) running natively on QRT: run by the
// QEMU test as "js /share/tests/js-test.js".  Throws (js exits with 1) on any mismatch.
const results = [];
function check(what, got, want) {
    const ok = JSON.stringify(got) === JSON.stringify(want);
    results.push(`${what}: ${ok ? "ok" : "FAILED (" + JSON.stringify(got) + ")"}`);
    if (!ok) throw new Error(`js-test: ${what} gave ${JSON.stringify(got)}, wanted ${JSON.stringify(want)}`);
}

// the language
class Point { #x; constructor(x, y) { this.#x = x; this.y = y; } get sum() { return this.#x + this.y; } }
check("classes, private fields", new Point(3, 4).sum, 7);
function* gen() { yield 1; yield 2; yield 3; }
check("generators, spread", [...gen()].map(x => x * 2), [2, 4, 6]);
const { a, ...rest } = { a: 1, b: 2, c: 3 };
check("destructuring", [a, Object.keys(rest)], [1, ["b", "c"]]);
check("BigInt", (2n ** 100n).toString(), "1267650600228229401496703205376");
check("Map/Set", [new Map([[1, "a"], [2, "b"]]).get(2), new Set([1, 1, 2, 3]).size], ["b", 3]);
const p = new Proxy({}, { get: (_, k) => `prop:${String(k)}` });
check("Proxy", p.hello, "prop:hello");
check("typed arrays", Array.from(new Uint8Array(new Float32Array([1.5]).buffer)), [0, 0, 192, 63]);
check("RegExp (named groups, lookbehind, unicode)", "2026-10-02".replace(/(?<y>\d{4})-(?<m>\d\d)-(?<d>\d\d)/u, "$<d>.$<m>.$<y>") + /(?<=\$)\d+/.exec("cost $42")[0], "02.10.202642");
check("JSON", JSON.parse(JSON.stringify({ x: [1, { y: null }], z: "é" })), { x: [1, { y: null }], z: "é" });
check("Array methods", [5, 1, 4].toSorted((x, y) => x - y).concat([1, 2, 3].findLast(x => x < 3)), [1, 4, 5, 2]);
check("String", ["abc".at(-1), "a-b-c".replaceAll("-", "+"), "  x ".trim().padStart(3, "*")], ["c", "a+b+c", "**x"]);

// ICU: normalisation, case mapping, collation, Intl formatting
check("normalize (ICU)", "é".normalize("NFC") === "é", true);
check("toUpperCase (ICU)", "straße".toUpperCase(), "STRASSE");
check("Intl.NumberFormat", new Intl.NumberFormat("de-DE").format(1234567.891), "1.234.567,891");
check("Intl.DateTimeFormat", new Intl.DateTimeFormat("en-US", { timeZone: "UTC", dateStyle: "long" }).format(new Date(Date.UTC(2026, 9, 2))), "October 2, 2026");
check("Intl.Collator", ["z", "ä", "a"].sort(new Intl.Collator("de").compare), ["a", "ä", "z"]);
check("Intl.PluralRules", new Intl.PluralRules("en-US").select(1), "one");
check("Intl.Segmenter", [...new Intl.Segmenter("en", { granularity: "word" }).segment("hi there")].filter(s => s.isWordLike).length, 2);

// the job queue (the js shell has no timers): promise jobs run after the script, in order
const order = [];
Promise.resolve().then(() => order.push("microtask"));
(async () => {
    await null;
    order.push("async");
    await Promise.all([1, Promise.resolve(2)]);
    check("promise jobs", order, ["script", "microtask", "async"]);
    for (const r of results) console.log(r);
    console.log("js: ok");
})().catch(e => console.log(`js: FAILED: ${e.message}`));
order.push("script");
