#!/usr/bin/env python3
"""
Aura Benchmark Baseline — IR vs Tree-walker Comparison
"""
import subprocess, json, sys, os, time, statistics

AURA = os.environ.get("AURA_BIN", "./build/aura")

CASES = [
    ("arithmetic",    "(+ 1 2 3 4 5 6 7 8 9 10)", "55"),
    ("fib-20",        "(letrec ((fib (lambda (n) (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2))))))) (fib 20))", "6765"),
    ("fact-10",       "(letrec ((fact (lambda (n) (if (= n 0) 1 (* n (fact (- n 1))))))) (fact 10))", "3628800"),
    ("closure-set!",  '(define (mk)(let((count 0))(lambda()(set! count(+ count 1))count)))(define c(mk))(c)(c)(c)(c)(c)', "5"),
    ("try-catch",     "(try (+ 1 2) (catch (e) 0))", "3"),
    ("self-ref-fact", "(define (fact n)(if(= n 0)1(* n(fact(- n 1)))))(fact 10)", "3628800"),
]

def time_run(cmd_args, code):
    start = time.perf_counter()
    r = subprocess.run([AURA] + cmd_args, input=code,
                       capture_output=True, text=True, timeout=30)
    return (time.perf_counter()-start)*1000, r.stdout.strip(), r.stderr.strip()

def bench_one(name, code, expected, iters=5):
    etimes, er = [], ""
    for _ in range(iters):
        t,r,_ = time_run([], code); etimes.append(t); er = r
    itimes, ir = [], ""
    for _ in range(iters):
        t,r,_ = time_run(["--ir"], code); itimes.append(t); ir = r
    d,i = statistics.median(etimes), statistics.median(itimes)
    return {"name":name,"tw_ms":f"{d:.1f}","ir_ms":f"{i:.1f}",
            "ratio":f"{d/i:.2f}x" if i>0 else "—",
            "tw_ok":"✅" if expected is None or er==expected else f"❌",
            "ir_ok":"✅" if expected is None or ir==expected else f"❌",
            "tw_val":er[:25],"ir_val":ir[:25]}

def main():
    print("="*65)
    print("  Aura Benchmark — IR vs Tree-walker")
    print("="*65)
    print(f"  {'Benchmark':18s} {'TW':>7s} {'IR':>7s} {'Ratio':>7s}  {'TW':4s} {'IR':4s} {'TW result':20s}")
    print(f"  {'-'*18} {'-'*7} {'-'*7} {'-'*7}  {'-'*4} {'-'*4} {'-'*20}")
    rs = []
    for n,c,e in CASES:
        r = bench_one(n,c,e)
        rs.append(r)
        print(f"  {r['name']:18s} {r['tw_ms']:>6s}ms {r['ir_ms']:>6s}ms {r['ratio']:>7s}  "
              f"{r['tw_ok']:4s} {r['ir_ok']:4s} {r['tw_val']:20s}")
    tw_t = sum(float(r['tw_ms']) for r in rs)
    ir_t = sum(float(r['ir_ms']) for r in rs)
    print(f"  {'─'*18} {'─'*7} {'─'*7} {'─'*7}  {'─'*4} {'─'*4}")
    print(f"  {'TOTAL':18s} {tw_t:>6.0f}ms {ir_t:>6.0f}ms {tw_t/ir_t:.2f}x")
    print(f"\n  ✅ IR is {tw_t/ir_t:.1f}x faster on pure workloads")
    print(f"  (stdlib require/import only work in default tree-walker path)")

    jp = os.path.join(os.path.dirname(__file__),"benchmark_baseline.json")
    with open(jp) as f: existing = json.load(f)
    existing["ir_vs_treewalker"] = rs
    with open(jp,"w") as f: json.dump(existing,f,indent=2)

if __name__ == "__main__":
    main()
