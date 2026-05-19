#!/usr/bin/env python3
"""Aura AI Agent — 两阶段增量开发 (EDSL Phase 2)

Phase 1 (exec):   写完整代码、跑通
Phase 2 (EDSL):   遇到错误后自动锁定 + query + mutate 精确修复
"""
import subprocess, json, sys, os, time, re, http.client, urllib.parse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ai_agent_prompt import build_system_prompt
from stdlib_inliner import inline_stdlib

AURA = os.environ.get("AURA_BIN", "./build/aura")
LLM_KEY = os.environ.get("LLM_API_KEY") or os.environ.get("OPENAI_API_KEY", "")
LLM_URL = os.environ.get("LLM_BASE_URL") or os.environ.get("OPENAI_BASE_URL", "https://api.openai.com/v1")
LLM_MODEL = (os.environ.get("LLM_MODEL") or os.environ.get("OPENAI_MODEL", "deepseek-v4-flash")).split("/")[-1]
MAX_ROUNDS = 25

SYSTEM_PROMPT = build_system_prompt() + """

When starting, run (api-reference) to see available primitives. This is always up to date.

## EDSL (Phase 2: precise fixes)

When Phase 1 code fails: use EDSL to fix. NEVER rewrite the whole code.

### IMPORTANT: (current-source) gives you the updated source after mutations
After any mutate:rebind / mutate:set-body / mutate:replace-value, ALWAYS:
1. (current-source) -> get the updated source as a string
2. Put the updated source in your response to show the new code

The updated source shows you exactly what the workspace contains after the AST transformation.

### Workflow
(set-code "(begin (define ...) (define ...) ...)")  ; Lock workspace
(current-source)              ; Get updated source after mutations
(query:find "func")               ; Find node ID
(query:children N)                ; See node structure
(mutate:rebind "func" "(define ...)")   ; Replace entire define
(mutate:set-body "name" "body")         ; Replace just the body
(mutate:replace-value 3 "42")           ; Replace a literal
(typecheck-current) + (eval-current)    ; Verify

### Example: fix a typo in recursive call


### Example: defmacro (Aura syntax)


### Rules
- Use (begin ...) for multi-expression set-code
- ALWAYS call (current-source) after mutations
- Only change the broken part, not the whole program
- Say DONE when correct.

"""


class AuraSession:
    def __init__(self):
        self.p = subprocess.Popen([AURA, "--serve"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True, bufsize=1)
    def send(self, cmd, timeout=30):
        self.p.stdin.write(json.dumps(cmd) + "\n")
        self.p.stdin.flush()
        deadline = time.time() + timeout
        while time.time() < deadline:
            l = self.p.stdout.readline()
            if l and l.startswith("{"):
                return json.loads(l)
        return {"status": "timeout"}
    def exec(self, code, timeout=30):
        # Inline stdlib imports so code runs through the IR pipeline
        code = inline_stdlib(code)
        return self.send({"cmd": "exec", "code": code}, timeout)
    def close(self):
        self.p.terminate(); self.p.wait()


def llm_call(msgs):
    p = urllib.parse.urlparse(LLM_URL)
    c = http.client.HTTPSConnection(p.netloc, timeout=120) if p.scheme == "https" else http.client.HTTPConnection(p.netloc, timeout=90)
    max_tok = 8000 if len(msgs) > 2 else 4000
    c.request("POST", p.path + "/chat/completions", json.dumps({
        "model": LLM_MODEL, "messages": msgs, "temperature": 0.3, "max_tokens": max_tok,
    }), {"Content-Type": "application/json", "Authorization": f"Bearer {LLM_KEY}"})
    r = c.getresponse()
    d = json.loads(r.read())
    c.close()
    finish_reason = d["choices"][0].get("finish_reason", "")
    msg = d["choices"][0]["message"]
    content = msg.get("content", "") or ""
    if not content and "reasoning_content" in msg:
        content = msg["reasoning_content"]
    # Auto-retry larger budget on truncation
    retries = 0
    while finish_reason == "length" and retries < 2 and max_tok < 16000:
        max_tok *= 2
        retries += 1
        c = http.client.HTTPSConnection(p.netloc, timeout=120)
        c.request("POST", p.path + "/chat/completions", json.dumps({
            "model": LLM_MODEL, "messages": msgs, "temperature": 0.3, "max_tokens": max_tok,
        }), {"Content-Type": "application/json", "Authorization": f"Bearer {LLM_KEY}"})
        r = c.getresponse()
        d = json.loads(r.read())
        c.close()
        finish_reason = d["choices"][0].get("finish_reason", "")
        msg = d["choices"][0]["message"]
    content = msg.get("content", "") or ""
    if not content and "reasoning_content" in msg:
        content = msg["reasoning_content"]
    if finish_reason == "length":
        content += f"\n\n## TRUNCATED — still truncated at {max_tok} tokens\n"
    return content


def extract_code(resp):
    """Extract Aura code or EDSL operations from LLM response.
    Returns ALL code blocks combined (not just the first)."""
    text = re.sub(r'<minimax:[^>]+>.*?</minimax:[^>]+>', '', text, flags=re.DOTALL)
    text = re.sub(r'<invoke[^>]*>.*?</invoke>', '', text, flags=re.DOTALL)
    text = re.sub(r'<parameter[^>]*>.*?</parameter>', '', text, flags=re.DOTALL)
    text = re.sub(r'</?[a-z_]+[^>]*>', '', text, flags=re.DOTALL)
    text = re.sub(r'<think>.*?</think>', '', resp, flags=re.DOTALL)
    blocks = []
    if "```" in text:
        for p in text.split("```"):
            lines = [l for l in p.strip().split("\n")]
            if lines and lines[0].lower() in ("aura", "scheme", "lisp", "racket", "python", "javascript", "", "cpp", "rust"):
                lines = lines[1:]
            c = "\n".join(l for l in lines if not l.startswith((";", "#lang", "#!"))).strip()
            if c and any(k in c for k in ("define", "require", "(+", "(begin", "lambda",
                                           "import", "set-code", "query:", "mutate:",
                                           "typecheck", "eval-current")):
                blocks.append(c)
        if blocks:
            return "\n\n".join(blocks)
    return ""


def main():
    if not LLM_KEY:
        print("Need LLM_API_KEY or OPENAI_API_KEY"); sys.exit(1)

    prompt = " ".join(sys.argv[1:]) if len(sys.argv) > 1 else input("> ")
    print(f"\n{'='*50}\nLLM: {LLM_MODEL}\nTask: {prompt}\n{'='*50}\n")

    aura = AuraSession()
    msgs = [{"role": "system", "content": SYSTEM_PROMPT}, {"role": "user", "content": prompt}]
    current_src = ""      # full source code of the program being developed
    in_phase2 = False

    empty_code_rounds = 0
    phase2_rounds = 0
    for rnd in range(1, MAX_ROUNDS + 1):
        phase = "Phase 2 EDSL" if in_phase2 else "Phase 1 exec"
        print(f"\n── Round {rnd} ({phase}) ──")
        print("  Thinking...", end=" ", flush=True)
        resp = llm_call(msgs)
        print("ok")

        code = extract_code(resp)
        if not code:
            last_line = resp.strip().split("\n")[-1].upper()
            if "DONE" in last_line and "TRUNCATED" not in resp:
                print("  DONE"); break
            empty_code_rounds += 1
            if empty_code_rounds >= 3:
                print("  ⚠️ LLM kept returning text without code — ending")
                break
            msgs.append({"role": "assistant", "content": resp})
            msgs.append({"role": "user", "content": "You did not include any code in your response. Please either write Aura code in a code block, or say DONE if the task is complete."})
            continue
        empty_code_rounds = 0

        print(f"  Code:\n    {code[:300]}")

        # Route: Phase 2 EDSL or Phase 1 exec
        is_edsl = code.startswith(("(set-code", "(query:", "(mutate:", "(typecheck", "(eval-current", "(current-source"))

        # Wrap multi-line EDSL in (begin ...)
        if is_edsl and "\n" in code:
            lines = [l.strip() for l in code.split("\n") if l.strip() and not l.startswith(";")]
            if len(lines) > 1:
                code = "(begin " + " ".join(lines) + ")"

        if is_edsl:
            in_phase2 = True
            phase2_rounds += 1
            if phase2_rounds > 10:
                print("  ⚠️ Too many Phase 2 EDSL rounds — ending")
                break
            if code.startswith("(set-code"):
                # Extract source from set-code to track our program
                m = re.match(r'\(set-code\s+"((?:[^"\\]|\\.)*)"', code)
                if m:
                    current_src = m.group(1)
            result = aura.exec(code)
        else:
            # Phase 1: regular Aura code
            # Save full source for potential EDSL recovery
            in_phase2 = False
            code_clean = re.sub(r'\(display\s+([^)]+)\)', r'\1', code)
            if not code_clean.startswith("(begin"):
                lines = [l.strip() for l in code_clean.split("\n") if l.strip() and not l.startswith(";")]
                if len(lines) > 1:
                    code_clean = "(begin " + " ".join(l for l in lines if not l.startswith(";")) + ")"
            current_src = code_clean  # track for EDSL recovery
            result = aura.exec(code_clean)

        # Handle result
        if result:
            status = result.get("status")
            value = result.get("value", result.get("msg", ""))
            print(f"  → {status}: {value[:150]}")

            last_line = resp.strip().split("\n")[-1].upper()

            if status == "ok":
                if in_phase2:
                    # After successful EDSL operation, get the updated source
                    src_result = aura.exec("(current-source)")
                    updated_src = ""
                    if src_result and src_result.get("status") == "ok":
                        updated_src = src_result.get("value", "").strip('\"')
                        current_src = updated_src
                    if updated_src:
                        fb = f"Phase 2 fix applied.\n\nUpdated source:\n{updated_src}\n\nSay DONE if correct."
                    else:
                        fb = f"Result: {value}\nPhase 2 fix applied. Say DONE if correct."
                else:
                    # Auto-test: try calling each defined function
                    test_results = []
                    all_ok = True
                    failed_funcs = []
                    for fname in re.findall(r'\(define\s+\(?([\w-]+)', current_src):
                        if fname in ("pi", "e", "data"):
                            continue
                        tested = False
                        for test_input in ("5", "10", '"hello"', '"x"'):
                            test_call = f"({fname} {test_input})"
                            tr = aura.exec(test_call)
                            if tr:
                                ts = tr.get("status")
                                tv = tr.get("value", tr.get("msg", ""))
                                test_results.append(f"  ({fname} {test_input}) → {ts}: {tv}")
                                tested = True
                                if ts != "ok":
                                    all_ok = False; failed_funcs.append(fname)
                                break
                        if not tested:
                            test_results.append(f"  {fname} → (no valid test)")
                            failed_funcs.append(fname)

                    test_blurb = "\n".join(test_results) if test_results else "  (no test ran)"
                    if all_ok and status == "ok" and "DONE" in last_line and "TRUNCATED" not in resp:
                        print("  DONE"); break

                    # Auto EDSL: lock workspace + query failing function nodes
                    edsl_nodes = ""
                    if not all_ok and current_src:
                        esc = current_src.replace("\\", "\\\\").replace('"', '\\"')
                        aura.exec(f'(set-code "{esc}")')
                        for ff in failed_funcs:
                            qr = aura.exec(f'(query:find "{ff}")')
                            if qr and qr.get("status") == "ok":
                                edsl_nodes += f"  {ff} node: {qr.get("value")}\n"
                    hint = ""
                    if edsl_nodes:
                        hint = f"Functions:\n{edsl_nodes}\nUse mutate:rebind to fix.\n"

                    fb = (
                        f"Result: {value}\n"
                        f"Auto-tests:\n{test_blurb}\n\n"
                        f"{hint}"
                        "Say DONE if correct. If tests failed, fix with EDSL."
                    )
            else:
                # ERROR → auto Phase 2 EDSL: set-code + query for context
                in_phase2 = True

                # Auto-lock the failed source into workspace
                if current_src:
                    esc_src = current_src.replace('\\', '\\\\').replace('"', '\\"')
                    aura.exec(f'(set-code "{esc_src}")')

                # Try to query:find defined functions in the source
                funcs_found = []
                for func_name in re.findall(r'\(define\s+\(?(\w+)', current_src):
                    qr = aura.exec(f'(query:find "{func_name}")')
                    if qr and qr.get("status") == "ok":
                        funcs_found.append(f"  {func_name} → {qr.get('value')}")

                edsl_hint = ""
                if funcs_found:
                    edsl_hint = "Functions in workspace:\n" + "\n".join(funcs_found) + "\n\n"

                # Get current source for context
                src_result = aura.exec("(current-source)")
                current_ws_src = ""
                if src_result and src_result.get("status") == "ok":
                    current_ws_src = src_result.get("value", "").strip('\"')

                fb = (
                    f"ERROR: {value}\n\n"
                    f"Current source: {current_ws_src or current_src[:400]}\n\n"
                    f"{edsl_hint}"
                    "SWITCH TO PHASE 2 EDSL NOW.\n"
                    "1. (set-code \"...\") — lock current source\n"
                    "2. (query:find \"func-name\") — find the broken function\n"
                    "3. (mutate:rebind \"func\" \"(define (func ...) ...)\") — fix with exact code\n"
                    "4. (eval-current) — verify the fix\n"
                    "Do NOT rewrite the whole program. Only change the broken part.\n"
                )
        else:
            print("  → no response")
            fb = "No response. Try set-code to lock workspace."

        msgs.append({"role": "assistant", "content": resp})
        msgs.append({"role": "user", "content": fb})

        # History truncation: keep system + original task + last 3 rounds
        # [0] = system, [1] = user task, [2..] = rounds
        if len(msgs) > 10:
            keep = [msgs[0], msgs[1]]  # system + task
            # Keep last 4 pairs (8 messages: 4 resp + 4 fb) = last 4 rounds
            keep.extend(msgs[-8:])
            msgs = keep
            print(f"  (history trimmed to {len(msgs)} messages)")

    aura.close()
    print(f"\n{'='*50}\nDone after {rnd} rounds\n{'='*50}")


if __name__ == "__main__":
    main()
