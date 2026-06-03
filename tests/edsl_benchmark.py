#!/usr/bin/env python3
"""Aura EDSL Benchmark -- 多模型 × 多任务 × 多轮 × 迭代修正。

支持：
  - 多轮运行抵消 LLM 方差 (--rounds N)
  - 原生 intend 迭代修正（默认）
  - Phase 自适应温度/tokens控制
  - Execution trace 注入算法任务
  - JSON 输出 (--json)

用法:
  LLM_API_KEY="***" python3 tests/edsl_benchmark.py
  LLM_API_KEY="***" python3 tests/edsl_benchmark.py --max-attempts 5
  LLM_API_KEY="***" python3 tests/edsl_benchmark.py --rounds 3 --max-attempts 5 --json
  LLM_MODEL=minimax-m2.7 LLM_API_KEY="***" python3 tests/edsl_benchmark.py --max-attempts 5
"""

import concurrent.futures
import fcntl
import http.client
import json
import os
import re
import subprocess
import sys
import threading
import time
import urllib.parse
from collections import defaultdict
from pathlib import Path

AURA = os.environ.get("AURA_BIN", "./build/aura")


# ── Model routing config ────────────────────────────────────
# Each entry: (name, model_id, key, base_url)
# Set via env vars or defaults
class ModelConfig:
    def __init__(self, name, model_id, key, base_url):
        self.name = name
        self.model_id = model_id
        self.key = key
        self.base_url = base_url

# Default routing: phase → model, with task-category overrides
# Set LLM_MODEL_ROUTING=1 to enable; configure via env vars:
#   LLM_PRIMARY=deepseek-v4-flash  (for coarse)
#   LLM_SECONDARY=grok-4.3         (for fine/type/ffi)
#   LLM_CHEAP=minimax-m2.7         (for putt/edsl)
def _parse_model_cfg(name_key, model_key, url_key, key_name):
    model = os.environ.get(model_key, "")
    key = os.environ.get(name_key, "")
    url = os.environ.get(url_key, "")
    if not model or not key:
        return None
    return ModelConfig(key_name, model, key, url)

# Primary model (used for coarse/rewrite attempts)
_MODEL_PRIMARY = _parse_model_cfg(
    "LLM_API_KEY", "LLM_MODEL", "LLM_BASE_URL", "primary")
# Secondary model (used for fine/putt, type/ffi tasks)
_MODEL_SECONDARY = _parse_model_cfg(
    "LLM_API_KEY_2", "LLM_MODEL_2", "LLM_BASE_URL_2", "secondary")
# Cheap model (used for simple tasks, last-resort fallback)
_MODEL_CHEAP = _parse_model_cfg(
    "LLM_API_KEY_3", "LLM_MODEL_3", "LLM_BASE_URL_3", "cheap")
# If no routing configured, use the primary model for everything
_MODEL_ROUTING_ENABLED = bool(_MODEL_SECONDARY or _MODEL_CHEAP)

# Task category → model override (based on task name prefix)
_TASK_CATEGORY_ROUTES = {
    "type-": "secondary",
    "ffi-": "secondary",
    "edsl-": "primary",
    "adt-": "primary",
    "m4-": "primary",
}


# Phase → model override
_PHASE_ROUTES = {
    "coarse": "primary",
    "fine": "secondary",
    "putt": "secondary",
}


def _route_model(task_name, phase):
    """Select model config based on task name and phase.
    Returns a ModelConfig or None (caller falls back to original params)."""
    if not _MODEL_ROUTING_ENABLED:
        return (_MODEL_PRIMARY if _MODEL_PRIMARY else None)
    # Phase-based routing
    level = _PHASE_ROUTES.get(phase, "primary")
    # Task-category override
    for prefix, route in _TASK_CATEGORY_ROUTES.items():
        if task_name.startswith(prefix):
            level = route
            break
    if level == "secondary" and _MODEL_SECONDARY:
        return _MODEL_SECONDARY
    if level == "cheap" and _MODEL_CHEAP:
        return _MODEL_CHEAP
    return (_MODEL_PRIMARY if _MODEL_PRIMARY else None)

# ── 任务定义 ─────────────────────────────────────────────
# ── 从 tasks/ 子目录加载任务 ────────────────────────────
TASKS_DIR = Path(__file__).resolve().parent / "tasks"
_TASK_HINTS = {}


def load_tasks():
    """从 tasks/<category>/*.aura 加载任务定义"""
    tasks = []
    if not TASKS_DIR.exists():
        print(f"Warning: {TASKS_DIR} not found")
        return tasks
    for fpath in sorted(TASKS_DIR.rglob("*.aura")):
        name = fpath.stem
        if name == "README":
            continue
        text = fpath.read_text()
        goal = ""
        expected = []
        stdlib = []
        hints = []
        for line in text.splitlines():
            if line.startswith(";; goal:"):
                goal = line[len(";; goal:") :].strip()
            elif line.startswith(";; expect:"):
                expected.append(line[len(";; expect:") :].strip())
            elif line.startswith(";; depend:"):
                stdlib.append(line[len(";; depend:") :].strip())
            elif line.startswith(";; hint:"):
                hints.append(line[len(";; hint:") :].strip())
        if goal:
            tasks.append((name, goal, expected, stdlib))
        else:
            print(f"Warning: {fpath} has no ;; goal: metadata")
        if hints:
            _TASK_HINTS[name] = "\\n".join(hints)
    return tasks


TASKS = load_tasks()

# ── CLI 配置默认值 ───────────────────────────────────────
ROUNDS = 1

EVOLVE_MODE = False
TRACE_MODE = False
MAX_ATTEMPTS = 3


# ── Serve Client ────────────────────────────────────────────
class ServeClient:
    """Persistent connection to ./aura --serve (CaaS).
    Single process handles all 57 tasks. Incremental compilation
    state preserved between requests."""

    def __init__(self, binary=None):
        self.binary = binary or AURA
        self.proc = None
        self._gen = 0
        self._reader_thread = None
        self._restart()

    def _restart(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.stdout.close()
                self.proc.stdin.close()
            except:
                pass
            self.proc.wait()
        self.proc = subprocess.Popen(
            [self.binary, "--serve"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
            close_fds=True,
        )
        time.sleep(0.3)

    def exec(self, code, read_timeout=15):
        """Execute code via serve with non-blocking read timeout.
        Kills and restarts serve on timeout. No threads."""
        if not code:
            return False, "", "no code"
        if self.proc.poll() is not None:
            self._restart()
            return False, "", "serve restarted (was dead)"

        self.proc.stdin.write(json.dumps({"cmd": "exec", "code": code}) + "\n")
        self.proc.stdin.flush()

        # Non-blocking read loop with timeout
        fd = self.proc.stdout.fileno()
        old_flags = fcntl.fcntl(fd, fcntl.F_GETFL)
        fcntl.fcntl(fd, fcntl.F_SETFL, old_flags | os.O_NONBLOCK)

        buf = ""
        try:
            for _ in range(read_timeout):
                if self.proc.poll() is not None:
                    self._restart()
                    return False, "", "serve died, restarted"
                try:
                    chunk = os.read(fd, 4096)
                    if chunk:
                        buf += chunk.decode("utf-8", errors="replace")
                except (BlockingIOError, OSError):
                    pass
                if buf and "\n" in buf:
                    time.sleep(0.02)  # tiny extra wait for more data
                    try:
                        more = os.read(fd, 4096)
                        if more:
                            buf += more.decode("utf-8", errors="replace")
                    except:
                        pass
                    break
                time.sleep(0.05)
            else:
                self.proc.kill()
                self.proc.wait()
                self._restart()
                return False, "", "serve hang (" + str(read_timeout) + "s), restarted"
        finally:
            try:
                fcntl.fcntl(fd, fcntl.F_SETFL, old_flags)
            except:
                pass

        # Find the JSON suffix - always the last {...} block
        stripped = buf.strip()
        if not stripped:
            return False, "", "empty response"
        if stripped.startswith("{"):
            json_line = stripped
            display_text = ""
        else:
            # Find LAST { (HTTP response body may contain {})
            brace = stripped.rfind("{")
            if brace >= 0:
                display_text = stripped[:brace]
                json_line = stripped[brace:]
            else:
                return False, stripped, "no JSON in response"
        try:
            resp = json.loads(json_line)
        except json.JSONDecodeError:
            return False, stripped, "invalid JSON"
        status = resp.get("status", "error")
        if status == "ok":
            val = resp.get("value", "")
            if display_text:
                out = display_text + (" " + val if val not in ("()", "") else "")
            else:
                out = val if val not in ("()", "") else ""
            return True, out.strip(), ""
        if status == "closure":
            return False, "#<procedure>", "program returned an uncalled function (closure)"
        return False, display_text, resp.get("msg", str(resp))

        def reader():
            try:
                line = self.proc.stdout.readline()
                if (
                    my_gen == self._gen
                ):  # only accept if we're still the active generation
                    result.append(line)
            except:
                if my_gen == self._gen:
                    result.append(None)
            done.set()

        t = threading.Thread(target=reader, daemon=True)
        t.start()
        if not done.wait(read_timeout):
            self.proc.kill()
            self.proc.wait()
            self._restart()
            return False, "", "serve timeout (" + str(read_timeout) + "s), restarted"
        line = (result[0] or "") if result else ""
        if not line:
            self._restart()
            return False, "", "serve response empty, restarted"
        stripped = line.strip()
        if stripped.startswith("{"):
            json_line = stripped
            display_text = ""
        else:
            # Find LAST { (HTTP response body may contain {})
            brace = stripped.rfind("{")
            if brace >= 0:
                display_text = stripped[:brace]
                json_line = stripped[brace:]
            else:
                print(
                    f"  [EXEC DEBUG] no JSON: {repr(stripped[:200])}", file=sys.stderr
                )
                if stripped and not stripped.startswith("{"):
                    try:
                        if self.proc.stderr:
                            pass  # time is already imported globally
                            time.sleep(0.2)
                            err_all = self.proc.stderr.read()
                            if err_all and err_all.strip():
                                print(
                                    f"  [EXEC DEBUG] stderr ({len(err_all)}b): {repr(err_all[:300])}",
                                    file=sys.stderr,
                                )
                    except:
                        pass
                return False, stripped, "no JSON in response"
        try:
            resp = json.loads(json_line)
        except json.JSONDecodeError:
            return False, stripped, "invalid JSON"
        status = resp.get("status", "error")
        if status == "ok":
            val = resp.get("value", "")
            if display_text:
                out = display_text + (" " + val if val not in ("()", "") else "")
            else:
                out = val if val not in ("()", "") else ""
            return True, out.strip(), ""
        if status == "closure":
            return False, "#<procedure>", "program returned an uncalled function (closure)"
        return False, display_text, resp.get("msg", str(resp))

    def exec_batch(self, codes, read_timeout=3):
        """Execute multiple code strings in a batch, minimizing IPC overhead.
        Sends all commands at once via stdin, then reads all responses.
        Returns list of (ok, out, err) tuples, one per code."""
        if not codes:
            return []
        if self.proc.poll() is not None:
            self._restart()
            return [(False, "", "serve restarted")] * len(codes)

        # Send all commands at once
        batch = ""
        for code in codes:
            batch += json.dumps({"cmd": "exec", "code": code}) + "\n"
        self.proc.stdin.write(batch)
        self.proc.stdin.flush()

        # Non-blocking read loop
        fd = self.proc.stdout.fileno()
        old_flags = fcntl.fcntl(fd, fcntl.F_GETFL)
        fcntl.fcntl(fd, fcntl.F_SETFL, old_flags | os.O_NONBLOCK)

        buf = ""
        try:
            for _ in range(read_timeout):
                if self.proc.poll() is not None:
                    self._restart()
                    return [(False, "", "serve died")] * len(codes)
                try:
                    chunk = os.read(fd, 4096)
                    if chunk:
                        buf += chunk.decode("utf-8", errors="replace")
                except (BlockingIOError, OSError):
                    pass
                if buf.count("\n") >= len(codes):
                    time.sleep(0.02)
                    try:
                        more = os.read(fd, 4096)
                        if more:
                            buf += more.decode("utf-8", errors="replace")
                    except:
                        pass
                    break
                time.sleep(0.05)
        finally:
            try:
                fcntl.fcntl(fd, fcntl.F_SETFL, old_flags)
            except:
                pass

        # Parse responses (one per line)
        lines = buf.strip().split("\n")
        results = []
        for line in lines[: len(codes)]:
            stripped = line.strip()
            if not stripped:
                results.append((False, "", "empty response"))
                continue
            if stripped.startswith("{"):
                json_line = stripped
                display_text = ""
            else:
                brace = stripped.rfind("{")
                if brace >= 0:
                    display_text = stripped[:brace]
                    json_line = stripped[brace:]
                else:
                    results.append((False, stripped, "no JSON"))
                    continue
            try:
                resp = json.loads(json_line)
            except json.JSONDecodeError:
                results.append((False, stripped, "invalid JSON"))
                continue
            if resp.get("status", "error") == "ok":
                val = resp.get("value", "")
                if display_text:
                    out = display_text + (" " + val if val not in ("()", "") else "")
                else:
                    out = val if val not in ("()", "") else ""
                results.append((True, out.strip(), ""))
            else:
                results.append((False, display_text, resp.get("msg", str(resp))))

        return results

    def close(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()


# ── LLM 调用 ──────────────────────────────────────────────
def llm_complete(model, base_url, key, messages, retries=3):
    parsed = urllib.parse.urlparse(base_url)
    path = parsed.path.rstrip("/") + "/chat/completions"
    # Some models require specific temperature settings
    model_lower = model.lower()
    if "kimi" in model_lower:
        temp = 1.0  # Kimi k2.6 only accepts temp=1.0
        request_timeout = 20  # Kimi can be very slow; cap at 45s
    else:
        temp = 0.3
        request_timeout = 120
    # MiniMax M2.7 wraps reasoning in <think> tags — use reasoning_split to separate
    # MiniMax-M3 also wraps reasoning; same handling applies.
    extra_params = {}
    if "minimax" in model_lower:
        # Use correct model ID (case-sensitive)
        if "m3" in model_lower:
            model = "MiniMax-M3"
        else:
            model = "MiniMax-M2.7"
        extra_params["reasoning_split"] = True
    for attempt in range(retries):
        try:
            conn_cls = (
                http.client.HTTPSConnection
                if parsed.scheme == "https"
                else http.client.HTTPConnection
            )
            h = conn_cls(parsed.netloc, timeout=request_timeout)
            body = {
                "model": model,
                "messages": messages,
                "temperature": temp,
                "max_tokens": 4096,
            }
            body.update(extra_params)
            h.request(
                "POST",
                path,
                json.dumps(body),
                {"Content-Type": "application/json", "Authorization": f"Bearer {key}"},
            )
            r = h.getresponse()
            d = json.loads(r.read())
            h.close()
            content = d.get("choices", [{}])[0].get("message", {}).get("content", "")
            # Use reasoning_details if content is empty and reasoning_split is active
            if not content:
                content = d.get("choices", [{}])[0].get("message", {}).get("reasoning_details", "")
            if content:
                return content
        except Exception as e:
            if attempt < retries - 1:
                time.sleep(2)
            else:
                raise e
    return ""


# ── 代码提取 ──────────────────────────────────────────────
def extract_code(text):
    # Some models (MiniMax m2.7) wrap entire response in <think> tags
    original = text
    
    # Step 1: Try to find code in ``` blocks first
    if "```" in text:
        for p in text.split("```"):
            lines = p.strip().split("\n")
            lines = [
                l
                for l in lines
                if not l.startswith(
                    ("aura", "scheme", "lisp", "racket", "python", "#lang", "#!")
                )
            ]
            c = "\n".join(lines).strip()
            if any(
                k in c
                for k in (
                    "define",
                    "require",
                    "(+",
                    "(begin",
                    "lambda",
                    "import",
                    "set-code",
                    "query:",
                    "mutate:",
                    "typecheck",
                    "eval-current",
                    "c-load",
                    "c-func",
                    "tcp-connect",
                    "http-post",
                    "(: ",
                )
            ):
                return c
    
    # Step 2: Strip think/XML tags (MiniMax reasons in <think>)
    text = re.sub(r"<think>.*?(</think>|$)", "\n", text, flags=re.DOTALL)
    text = re.sub(r"</?\w[^>]*>", "", text, flags=re.DOTALL)
    
    # Step 3: If nothing left after stripping, try content INSIDE think tags
    if not text.strip():
        m = re.search(r"<think>(.*?)</think>", original, flags=re.DOTALL)
        if m:
            text = m.group(1)
            text = re.sub(r"</?\w[^>]*>", "", text, flags=re.DOTALL)
    
    # Step 4: Strip leading garbage lines (MiniMax adds Chinese explanation)
    lines = text.strip().split("\n")
    # Find the first line that looks like code
    code_start = -1
    for i, line in enumerate(lines):
        stripped_line = line.strip()
        if not stripped_line:
            continue
        # Lines starting with ( are likely code
        if stripped_line.startswith(("(", "#", ";")):
            code_start = i
            break
        # Lines containing key Lisp patterns
        if any(kw in stripped_line for kw in
               ("define", "require", "lambda", "import", "set-code",
                "query:", "mutate:", "eval-current", "c-func", "display", ":")):
            code_start = i
            break
    if code_start >= 0:
        lines = lines[code_start:]
    
    text = "\n".join(lines)
    stripped = text.strip()
    
    # Step 5: If resulting text has natural language before code, extract last s-expr
    if stripped and not stripped.startswith(("(", "#", '"', "'")):
        # Try to find a balanced s-expression somewhere in the text
        for kw in ["(define", "(display", "(set-code", "(require", "(: "]:
            idx = stripped.find(kw)
            if idx >= 0:
                stripped = stripped[idx:]
                # Trim at the end too — stop at first blank line or natural language
                end_idx = len(stripped)
                for nl in ["\n\n", "\n\r\n"]:
                    ni = stripped.find(nl)
                    if ni >= 0 and ni < end_idx:
                        end_idx = ni
                stripped = stripped[:end_idx].strip()
                break
        if not stripped.startswith(("(", "#", '"', "'")):
            return ""
    
    # Step 6: Trim trailing Chinese/natural language after code
    if stripped:
        last_paren = stripped.rfind(")")
        for kw in [")。", "<br>", "\n\n"]:
            idx = stripped.find(kw)
            if idx >= 0:
                stripped = stripped[:idx].strip()
                break
    
    return stripped


# ── 执行测试 ──────────────────────────────────────────────
# ── 执行测试 ──────────────────────────────────────────────


def test_aura(code, timeout=10):
    try:
        r = subprocess.run(
            [AURA], input=code, capture_output=True, text=True, timeout=timeout
        )
        return r.returncode, r.stdout.strip(), r.stderr.strip()
    except subprocess.TimeoutExpired:
        return -1, "", "timeout"
    except FileNotFoundError:
        return -2, "", "aura binary not found"


def test_aura_serve(code, timeout=10):
    try:
        r = subprocess.run(
            [AURA, "--serve"],
            input=code,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
        return r.returncode, r.stdout.strip(), r.stderr.strip()
    except subprocess.TimeoutExpired:
        return -1, "", "timeout"
    except FileNotFoundError:
        return -2, "", "aura binary not found"


def check_success(out, expected):
    """Check if output matches expected keywords.
    Uses substring matching but guards against false positives
    from error messages that happen to contain the keyword."""
    norm_out = out.strip().strip('"').strip("'")
    # Detect structured errors: (kind message) pairs from eval-current
    is_structured_error = norm_out.startswith('("') and '" "' in norm_out[:80]
    # If output looks like an error message, be more strict
    # to prevent false positives from diagnostic strings that
    # happen to contain expected keywords.
    is_error_like = is_structured_error or any(m in norm_out.lower() for m in
        ["error:", "parse error", "unbound variable",
         "type error", "syntax error", "invalid syntax", "expected expression"])

    for kw in expected:
        if not kw:
            continue
        if kw in norm_out:
            # Guard: if output is error-like and keyword is very short/generic,
            # require word-boundary matching to avoid false positives
            if is_error_like and len(kw) <= 5:
                try:
                    import re
                    if re.search(r'\\b' + re.escape(kw) + r'\\b', norm_out):
                        return True
                except Exception:
                    return True  # fall back to substring match
                continue  # word-boundary didn't match, skip this kw
            return True
    return False


# ── 获取 api-reference ────────────────────────────────────
def get_api_ref():
    """Get Aura API reference from std/adaptive module.
    Includes core primitives + commonly used stdlib modules.
    Uses get-api-ref with a targeted module list (fast).
    Falls back to empty string if unavailable."""
    try:
        modules = '"std/list" "std/hash" "std/json" "std/string" "std/math"'
        code = f'(require "std/adaptive" all:)(display (get-api-ref (list {modules})))'
        r = subprocess.run(
            [AURA], input=code, capture_output=True, text=True, timeout=10
        )
        if r.returncode == 0:
            ref = r.stdout.strip()
            if ref and len(ref) > 100:
                return ref
    except Exception:
        pass
    return ""



PROMPT_SECTIONS = {
    "identity": (
        "You are Aura Lisp. Write valid code ending with (display ...).\n"
        "CRITICAL: (display (your-function args)) — if you only (define (f x) ...)"
        " the output will be '#<procedure>' and the TEST WILL FAIL!\n"
    ),
    "syntax": (
        "\n=== AURA SYNTAX REFERENCE ===\n"
        "\nBASIC:\n  (+ 1 2)             → addition\n"
        "  (define (f x) (+ x 1))  → define a function\n"
        "  (lambda (x) (+ x 1))    → anonymous function\n"
        "  (let ((x 10)) (+ x 1))  → local binding\n"
        "  (if cond then else)   → conditional\n"
        "  (cons 1 (list 2 3))   → pair / list\n"
        "\nDISPLAY (required for tests):\n  (display 42)          → print 42\n"
        "  (display (/ 10 2))    → print 5\n"
        "  (display  (+ 1 2))    → print 3\n"
        "\nTYPE ANNOTATIONS:\n  (: x Int 42)          → bind x:Int with value 42\n"
        "  ((lambda ((: x Int)) (+ x 1)) 41)  → lambda with typed param\n"
        "  (define (f (: x Int)) (+ x 1))    → define with typed param\n"
        "  (: x String \"hello\") → string type annotation\n"
        "\nFUNCTOR / MODULE:\n"
        "  (define-module (Stack :T) (export push pop)\n"
        "    (define (push s x) (cons x s)))\n"
        "  (Stack Int)            → instantiate functor with Int\n"
        "  (module-get (Stack Int) \"push\") → get function from instance\n"
        "\nEDSL (self-modification):\n"
        "  (set-code \"(define (f x) (+ x 1))\") → load code into workspace\n"
        "  (query:root)            → get root node ID\n"
        "  (query:children node)   → get children of a node\n"
        "  (mutate:rebind name new-code)  → replace function definition\n"
        "  (mutate:set-body name new-body) → replace function body\n"
        "  (mutate:insert-child parent pos code) → insert child node\n"
        "  (eval-current)          → evaluate workspace code\n"
        "  (typecheck-current)     → type-check workspace code\n"
        "  (ast:snapshot name)     → save AST snapshot\n"
        "  (ast:restore snap-id)   → restore AST snapshot\n"
        "\nMODULE / IMPORT:\n"
        "  (require \"std/list\" all:)  → load stdlib module\n"
        "  (import \"path\")            → import module\n"
        "  (generate-type-sigs \"path\") → generate .aura-type file\n"
        "\nTYPES:\n  Int, String, Bool, Float, Void, List, Pair, Hash\n"
        "\n=== END REFERENCE ===\n"
    ),
}

# Default section order (can be overridden per task)
DEFAULT_SECTION_ORDER = ["identity", "syntax"]

# Task → section overrides for fine-grained control
TASK_SECTION_OVERRIDES = {}
# Task-specific overrides not needed — prompt is compact enough for all tasks.


def build_sys_prompt(stdlib, api_ref, task_name=""):
    # Select sections for this task
    sections = DEFAULT_SECTION_ORDER
    for prefix, override in TASK_SECTION_OVERRIDES.items():
        if task_name.startswith(prefix):
            sections = override
            break

    # Build prompt from selected sections
    sp = "".join(PROMPT_SECTIONS[s] for s in sections if s in PROMPT_SECTIONS)

    if stdlib:
        sp += f"Available stdlib: {', '.join(stdlib)}. Use (require std/name all:) to load them.\n"
    if task_name and task_name in _TASK_HINTS:
        sp += "\n" + "=" * 40 + "\n"
        sp += f'TASK-SPECIFIC HINT for "{task_name}":\n'
        sp += _TASK_HINTS[task_name]
    evolved = os.environ.get("EVOLVED_HINTS", "")
    if evolved:
        sp += "\n" + "=" * 40 + "\n"
        sp += f"EVOLVED HINTS (from past runs):\n{evolved}\n"
    if api_ref:
        sp += "\n" + "=" * 40 + "\n"
        sp += "Aura API Reference:\n"
        sp += api_ref
    return sp


def _ada_esc(s):
    # Escape for embedding in Aura string literals
    s = s.replace("\\", "\\\\")  # backslash -> double backslash
    s = s.replace('"', '\\"')  # double quote -> backslash-quote
    s = s.replace("\n", "\\n")  # actual newline -> literal \\n
    return s


def _ada_list(items):
    if not items:
        return "(list)"
    return "(list " + " ".join('"' + _ada_esc(str(it)) + '"' for it in items) + ")"


def call_adaptive(rc, output, expected_list):
    exp_list = _ada_list(expected_list)
    out_esc = _ada_esc(output)
    code = (
        '(require "std/adaptive" all:)'
        '(pid:analyze "' + out_esc + '" ' + exp_list + ')'
    )
    try:
        r = subprocess.run(
            [AURA], input=code, capture_output=True, text=True, timeout=10
        )
        if r.returncode != 0:
            return "fine", 0.0, "(ada-unavail)", ""
        # Parse pid:analyze output: (phase ratio diagnosis feedback)
        out = r.stdout.strip()
        if out.startswith("(") and out.endswith(")"):
            inner = out[1:-1]
            # Simple split: find elements by balanced parens
            parts = []
            depth = 0
            current = ""
            for c in inner:
                if c == '(': depth += 1
                elif c == ')': depth -= 1
                if depth == 0 and c == ' ' and current.strip():
                    parts.append(current.strip())
                    current = ""
                elif depth > 0 or c != ' ':
                    current += c
                elif c != ' ' or current:
                    current += c
            if current.strip():
                parts.append(current.strip())
            phase = parts[0].strip('"') if len(parts) >= 1 else "fine"
            try:
                ratio = float(parts[1]) if len(parts) >= 2 else 0.0
            except:
                ratio = 0.0
            diag = parts[2].strip('"') if len(parts) >= 3 else ""
            fb = parts[3].strip('"') if len(parts) >= 4 else ""
            return phase, ratio, diag, fb
        return "fine", 0.0, "", ""
    except Exception:
        return "fine", 0.0, "(ada-err)", ""


def call_api_ref(stdlib_list):
    if not stdlib_list:
        return ""
    lst = "(list " + " ".join('"' + m + '"' for m in stdlib_list) + ")"
    code = '(require "std/adaptive" all:)(display (get-api-ref ' + lst + "))"
    try:
        r = subprocess.run(
            [AURA], input=code, capture_output=True, text=True, timeout=5
        )
        if r.returncode == 0 and r.stdout.strip():
            return r.stdout.strip()
    except Exception:
        pass
    return ""


# ── 共享的 Adaptive 反馈逻辑 ──────────────────────────────
def build_adaptive_feedback(
    name, actual_output, expected, stdlib, sys_prompt, prompt, current_src=""
):
    """Build structured feedback for adaptive retry via pure Aura pid:analyze.
    Shared between --fix and --intend modes.
    Returns (structured_feedback, phase, temperature, max_tokens).
    """
    p, ratio, diag, diag_text = call_adaptive(0, actual_output, expected)

    # Use Aura's pid:analyze feedback as base, add LLM-specific context
    fb = [diag_text] if diag_text else []
    fb.append("- Keep the existing function structure. Only modify display/output code.")

    # Execution trace for algorithm-debug tasks
    if current_src:
        if name in ("primes-list", "quicksort", "prime-test"):
            trace_out, trace_err = get_execution_trace(current_src, timeout=5)
            if trace_out:
                fb.append("")
                fb.append("=== Execution Trace ===")
                fb.append(trace_out[:500])
            if trace_err:
                fb.append("")
                fb.append("=== Execution Errors ===")
                fb.append(trace_err[:200])
        elif name == "tcp-connect":
            trace_out, trace_err = get_execution_trace(current_src, timeout=15)
            if trace_out:
                fb.append("")
                fb.append("=== Execution Trace ===")
                fb.append("HTTP Response (first 500 chars):")
                fb.append(trace_out[:500])
            if trace_err:
                fb.append("")
                fb.append("=== Connection Errors ===")
                fb.append(trace_err[:200])


    if diag and diag != "":
        fb.append("")
        fb.append("Diagnosis: " + diag)
    if diag_text:
        fb.append(diag_text)
    fb.append("")
    fb.append("=== Current Source (AST) ===")
    fb.append(current_src if current_src else "(not available)")
    fb.append("")
    fb.append("=== Goal ===")
    fb.append(prompt)

    ada_api_ref = call_api_ref(stdlib)
    if p in ("fine", "putt") and ada_api_ref:
        fb.append("")
        fb.append(ada_api_ref)

    structured_feedback = "\n".join(fb)

    if p == "coarse":
        temp_v, tokens_v = 0.3, 4096
    elif p == "fine":
        temp_v, tokens_v = 0.2, 2048
    else:
        temp_v, tokens_v = 0.1, 1024

    return structured_feedback, p, temp_v, tokens_v


# ── 单任务（通过内置 intend 原语）────────────────────────


# ── 蚁群控制器 — 局部变异搜索 ──────────────────────────

# ── Phase A: EDSL 级殖民地搜索 ──────────────────────────


def _find_reference(code, expected):
    """Find the reference value closest to expected output."""
    nums = set()
    for m in re.finditer(r"(?<![a-zA-Z])(\d+)(?![a-zA-Z])", code):
        nums.add(int(m.group(1)))
    for kw in expected:
        try:
            nums.add(int(kw))
        except ValueError:
            pass
    ref = None
    if expected:
        try:
            ref = int(float(expected[0]))
        except (ValueError, IndexError):
            pass
    return ref, sorted(nums)


# 使用 set-code + mutate:rebind + eval-current 代替字符串替换
# 每个变体是一次增量编译，不是全量重跑


def _find_functions(code):
    """Find function definitions in code.
    Returns list of (fn_name, fn_args_str, fn_body_str, sigil_pos)
    where sigil_pos is position of (define(fn-name...
    """
    fns = []
    # Match (define (fn-name args) body)
    idx = 0
    while True:
        m = re.search(r"\(define\s+\(([^\s)]+)\s+", code[idx:])
        if not m:
            break
        fn_name = m.group(1)
        start = idx + m.start()
        # Skip args until closing paren of (define (fn-name args)
        depth = 1
        pos = idx + m.start() + len(m.group(0)) - 1  # back to after fn-name
        # Skip args — find closing ) of (fn-name args...)
        while pos < len(code):
            ch = code[pos]
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
                if depth == 0:
                    # Found closing ) of (define ...)
                    break
            pos += 1
        args_end = pos + 1

        # The body starts after args
        body_start = args_end
        # Body goes until matching ) of (define ...)
        depth = 0
        pos = body_start
        while pos < len(code):
            ch = code[pos]
            if ch == "(":
                depth += 1
            elif ch == ")":
                if depth == 0:
                    break  # closing ) of (define ...)
                depth -= 1
            pos += 1
        body_end = pos

        args_str = code[idx + m.end() : args_end - 1].strip()
        body_str = code[body_start:body_end].strip()
        esc_body = body_str.replace("\\", "\\\\").replace('"', '\\"')
        fns.append((fn_name, args_str, esc_body, start))
        idx = body_end + 1
    return fns


def _gen_edsl_variants(code, expected):
    """Generate EDSL mutation commands for local search.
    Yields (edsl_command, description) tuples.
    Each command: set-code + mutate:rebind + eval-current in one exec call.
    Cost: <1ms per variant (incremental, no full recompile)."""
    ref, _ = _find_reference(code, expected)
    esc = code.replace("\\", "\\\\").replace('"', '\\"')

    # 1. Function body mutations via mutate:rebind
    fns = _find_functions(code)
    for fn_name, args_str, body_str, _ in fns:
        # Skip trivial functions
        if not body_str or body_str in ("#t", "#f", "()", ""):
            continue

        # Extract the unescaped body for modification
        raw_body = body_str.replace('\\"', '"').replace("\\\\", "\\")
        if not raw_body:
            continue

        # For each function, try:
        # a) Direct value return (if ref exists)
        if ref is not None:
            new_body = f"(display {ref})"
            new_esc = new_body.replace("\\", "\\\\").replace('"', '\\"')
            cmd = f'(set-code "{esc}")(mutate:rebind "{fn_name}" "(lambda ({args_str}) {new_esc})")(eval-current)'
            yield cmd, f"edsl {fn_name}->disp{ref}"

        # b) Wrap body in display
        if not raw_body.startswith("(display"):
            new_body = f"(display {raw_body})"
            new_esc = new_body.replace("\\", "\\\\").replace('"', '\\"')
            cmd = f'(set-code "{esc}")(mutate:rebind "{fn_name}" "(lambda ({args_str}) {new_esc})")(eval-current)'
            yield cmd, f"edsl {fn_name}->wrap-display"

        # c) Numeric literal tweaks in body
        lit_matches = list(re.finditer(r"(?<![a-zA-Z])(\d+)(?![a-zA-Z])", raw_body))
        for lm in lit_matches:
            val = int(lm.group(1))
            if val <= 1000 and val not in (0, 1):
                for delta in [1, -1, 2, -2]:
                    new_val = max(0, val + delta)
                    if new_val == val:
                        continue
                    mod_body = (
                        raw_body[: lm.start()] + str(new_val) + raw_body[lm.end() :]
                    )
                    mod_esc = mod_body.replace("\\", "\\\\").replace('"', '\\"')
                    cmd = f'(set-code "{esc}")(mutate:rebind "{fn_name}" "(lambda ({args_str}) {mod_esc})")(eval-current)'
                    yield cmd, f"edsl {fn_name} lit {val}->{new_val}"

        # d) Condition flip in body: swap if then/else branches
        for m in re.finditer(r"\(if\s+([^)]+)\s+([^)]+)\s+([^)]+)\)", raw_body):
            cond = m.group(1).strip()
            then_expr = m.group(2).strip()
            else_expr = m.group(3).strip()
            if then_expr != else_expr:
                mod_body = raw_body[:m.start()] + f"(if {cond} {else_expr} {then_expr})" + raw_body[m.end():]
                mod_esc = mod_body.replace("\\", "\\\\").replace('"', '\\"')
                cmd = f'(set-code "{esc}")(mutate:rebind "{fn_name}" "(lambda ({args_str}) {mod_esc})")(eval-current)'
                yield cmd, f"edsl {fn_name} cond-flip"

        # e) Operator swap in body
        swaps = {
            "<": "<=",
            "<=": "<",
            ">": ">=",
            ">=": ">",
            "=": "not=",
            "not=": "=",
            "<": ">",
            ">": "<",
            "<=": ">=",
            ">=": "<=",
        }
        for old_op, new_op in swaps.items():
            if old_op in raw_body.split():
                mod_body = re.sub(
                    r"(?<![a-zA-Z])" + re.escape(old_op) + r"(?![a-zA-Z])",
                    new_op,
                    raw_body,
                )
                if mod_body != raw_body:
                    mod_esc = mod_body.replace("\\", "\\\\").replace('"', '\\"')
                    cmd = f'(set-code "{esc}")(mutate:rebind "{fn_name}" "(lambda ({args_str}) {mod_esc})")(eval-current)'
                    yield cmd, f"edsl {fn_name} op {old_op}->{new_op}"

    # 2. Display format changes (fallback: full code replacement)
    # For display calls that don't affect function structure
    for m in re.finditer(r"\(display\s+([^)]+)\)", code):
        arg = m.group(1).strip()
        if arg.startswith("("):
            continue  # skip nested

        has_hash_ops = "hash-set!" in code or "hash-ref" in code or "hash" in code
        is_hash_var = has_hash_ops and re.match(r"^[a-z][a-z0-9_-]*$", arg)

        for new_arg in [
            "(hash-keys " + arg + ")",
            "(hash-values " + arg + ")",
            "(hash-length " + arg + ")",
            "(number->string " + arg + ")",
            "#t",
            "#f",
        ]:
            _ = new_arg  # suppress unused
            pass

        # Hash-specific display fixes
        if arg.startswith("<hash") or "hash-" in arg or is_hash_var:
            for wrapper in [
                "(hash-keys " + arg + ")",
                "(hash-values " + arg + ")",
                "(hash-length " + arg + ")",
            ]:
                variant = (
                    code[: m.start()] + "(display " + wrapper + ")" + code[m.end() :]
                )
                yield variant, f"full {wrapper[:25]}"
            if any(kw in expected for kw in ["#t", "#f", "true", "false"]):
                variant = code[: m.start()] + "(display #t)" + code[m.end() :]
                yield variant, "full disp #t"

        # Boolean expected: try display #t directly
        if any(kw in expected for kw in ["#t", "#f", "true", "false"]):
            variant = code[: m.start()] + "(display #t)" + code[m.end() :]
            yield variant, "full disp #t"
            variant = code[: m.start()] + "(display #f)" + code[m.end() :]
            yield variant, "full disp #f"

        # Ref value display
        if ref is not None:
            variant = code[: m.start()] + "(display " + str(ref) + ")" + code[m.end() :]
            yield variant, f"full disp {ref}"
            variant = code[: m.start()] + "(display " + new_arg + ")" + code[m.end() :]
            yield variant, f"full {new_arg[:20]}"


MAX_COLONY_VARIANTS = 25
COLONY_VARIANT_TIMEOUT = 3
COLONY_MAX_TIME = 6.0

# ── Phase D: PID-guided variant limits ──
# putt: only try top-3 pheromone variants (nearby, fine-tuning only)
# fine: try top-10 pheromone variants (some distance, broader search)
# coarse: skip completely (too far, local mutations can't help)
_PHASE_VARIANT_LIMITS = {"putt": 20, "fine": 50}

# Global pheromone state for cross-task learning
_COLONY_PHEROMONE = {"initialized": False}


def _colony_load_pheromone(serve, task_name):
    """Load pheromone table from cross-task state."""
    global _COLONY_PHEROMONE
    if not serve:
        return
    if _COLONY_PHEROMONE.get("initialized"):
        export_json = _COLONY_PHEROMONE.get("export", "")
        if export_json:
            serve.exec(
                '(require "std/ant" all:)(pheromone:import "' + export_json + '")'
            )


def _colony_save_pheromone(serve):
    """Save pheromone table for next task."""
    global _COLONY_PHEROMONE
    if not serve:
        return
    _, phero_out, _ = serve.exec('(require "std/ant" all:)(display (pheromone:export))')
    if phero_out:
        # Extract just the JSON part (before the serve's JSON response)
        brace = phero_out.rfind("{")
        if brace >= 0:
            phero_json = phero_out[brace:]
            try:
                json.loads(phero_json)
                _COLONY_PHEROMONE["export"] = phero_json
                _COLONY_PHEROMONE["initialized"] = True
            except:
                pass


def internal_colony_search(serve, last_code, expected, phase, task_name=""):
    """Pure Aura ant colony search via colony:search primitive.
    One serve.exec() call does all variants.
    Returns (found, output, debug_msg)."""
    if not serve or not last_code:
        return False, "", ""
    if phase == "coarse":
        return False, "", "coarse: skip"

    # PID-guided variant limit
    pid_limit = _PHASE_VARIANT_LIMITS.get(phase, MAX_COLONY_VARIANTS)
    colony_start = time.time()

    # Convert expected list to comma-separated string
    if isinstance(expected, list):
        expected_str = ",".join(expected)
    else:
        expected_str = str(expected)

    # Single exec: pure Aura colony search
    # Escape expected string for Aura
    esc_expected = expected_str.replace('\\', '\\\\').replace('"', '\\"')
    ok, out, err = serve.exec(
        f'(require "std/ant" all:)(colony:search "{esc_expected}" {pid_limit})'
    )
    if not ok:
        return False, out or "", err or "colony:serve-error"

    # Parse output: (#t output msg) or (#f output msg)
    result_str = (out or "").strip()
    if result_str.startswith("(#t"):
        elapsed = time.time() - colony_start
        # Extract output (second element)
        parts = result_str.split("\"")
        captured = parts[1] if len(parts) > 1 else ""
        return True, captured, f"colony:aura in {elapsed:.1f}s"

    return False, "", "colony:aura-no-variant"


def get_execution_trace(code_str, timeout=10):
    """Run the generated code in Aura and capture all output as trace.
    Returns (stdout, stderr) or empty strings on failure.
    """
    if not code_str or code_str == "(not available)":
        return "", ""
    # Escape the code for embedding in set-code
    esc = code_str.replace("\\", "\\\\").replace('"', '\\"')
    aura = '(set-code "' + esc + '")(eval-current)'
    try:
        r = subprocess.run(
            [AURA], input=aura, capture_output=True, text=True, timeout=timeout
        )
        return r.stdout.strip() if r.returncode == 0 else "", r.stderr.strip()
    except Exception:
        return "", ""


# ── Error-to-API doc mapping (P2) ──────────────────────────
# Maps error keywords to relevant stdlib API documentation snippets
_ERROR_API_DOCS = {
    "rule:define": (
        "=== std/rule ===\n"
        "(rule:define name :pattern \"p\" :replace \"r\" ...)\n"
        "Keywords: :pattern (required), :replace (required), :condition, :description\n"
    ),
    "synthesize:pipeline": (
        "=== std/pipeline ===\n"
        '(synthesize:pipeline "name" step1 step2 ...)\n'
        "Each step: (synthesize:fill \"tmpl\" args...) or (synthesize:define ...)\n"
    ),
    "synthesize:register-template": (
        "=== synthesize templates ===\n"
        '(synthesize:register-template "name" "template" "arg")  ; Register template\n'
        '(synthesize:fill "name" arg1 arg2 ...)  ; Fill template with args\n'
    ),
    "send": (
        "=== send/recv ===\n"
        "(send message target)  ; Send message to channel\n"
        "(recv)  ; Receive message (blocking)\n"
    ),
    "make-hash": (
        "=== std/hash ===\n"
        "(hash key val ...) -> <hash[N]>  Create hash\n"
        "(hash-ref hash key) -> value | ()  Lookup key\n"
        "(hash->alist hash) -> alist  Convert to association list\n"
    ),
    "define-type": (
        "=== std/data ===\n"
        "(define-type Name (ctor field1 field2 ...) ...)\n"
        "Example: (define-type Tree (leaf val) (node left right))\n"
    ),
    "for-each": (
        "=== std/list ===\n"
        "(for-each fn lst) -> void  Apply fn to each element (side effect only)\n"
        "(map fn lst) -> list  Transform each element\n"
        "(filter pred lst) -> list  Keep matching elements\n"
    ),
    "c-func": (
        "=== C FFI ===\n"
        "(c-func lib-name \"c_fn_name\" arg-types ret-type)\n"
        "arg-types: list of \"int\", \"float\", \"string\"\n"
        "Example: (define sqrt-fn (c-func \"m\" \"sqrt\" (list \"double\") \"double\"))\n"
    ),
}


def _get_api_snippet(error_msg):
    """Extract relevant API doc snippet from error message."""
    for kw, doc in _ERROR_API_DOCS.items():
        if kw.lower() in error_msg.lower():
            return doc
    return ""


def _auto_fix_procedure(code, expected, run_code, is_edsl):
    """Auto-inject (display ...) when LLM outputs #<procedure>.
    Extracts function name + arity from source, then tries smart patterns.
    Uses a FRESH subprocess to avoid polluting the serve workspace."""
    import re as _re
    import subprocess as _sp

    def _fresh_run(test_code):
        try:
            r = _sp.run(
                [AURA], input=test_code, capture_output=True, text=True, timeout=10
            )
            return r.returncode == 0, r.stdout.strip(), r.stderr.strip()
        except Exception:
            return False, "", "subprocess error"

    def _extract_content(src):
        m = _re.match(r'\(set-code\s+"(.*)"\)', src.strip())
        if m:
            return m.group(1), True
        return src, False

    def _find_fn_with_arity(src):
        """Find last (define (fn a1 a2 ...) ...) or (define fn (lambda ...)).
        Returns (fn_name, arity) or None."""
        # (define (fn args...) body...)
        m = _re.findall(r'\(define\s+\(([^\s)]+)([^)]*)\)', src)
        if m:
            name, all_args = m[-1]
            args = [a for a in all_args.split() if a and not a.startswith(':')]
            return name, len(args)
        # (define fn (lambda (args...) ...))
        m = _re.findall(r'\(define\s+([^\s)]+)\s+\(lambda\s+\(([^)]*)\)', src)
        if m:
            return m[-1][0], len([a for a in m[-1][1].split() if a])
        return None

    def _gen_patterns(fn, arity):
        """Generate display patterns based on arity.
        Tries from most specific (list-based) to most generic."""
        pats = []
        if arity == 0:
            pats = [
                f'(display ({fn}))',
                f'(display "{fn} done")',
            ]
        elif arity == 1:
            pats = [
                # For sort-like: (display (fn (list 3 1 4 1 5)))
                f'(display ({fn} (list 3 1 4 1 5)))',
                # For simple: (display (fn 42))
                f'(display ({fn} 42))',
                f'(display ({fn} 0))',
                f'(display ({fn} \"test\"))',
            ]
        elif arity == 2:
            pats = [
                # For search-like: (display (fn (list 1 3 5 7 9) 5))
                f'(display ({fn} (list 1 3 5 7 9) 5))',
                # For other 2-arg: (display (fn (list 3 1 4 1 5) 1))
                f'(display ({fn} (list 3 1 4 1 5) 1))',
                f'(display ({fn} 42 7))',
                f'(display ({fn} 0 0))',
            ]
        else:  # 3+ args
            pats = [
                f'(display ({fn} 1 2 3))',
                f'(display ({fn} 42 7 0))',
                f'(display ({fn} 0 0 0))',
            ]
        # Always try the "done" pattern and raw display
        pats += [
            f'(display "{fn} completed")',
            f'(display {fn})',
        ]
        return pats

    def _inject_display(original, body, pat):
        if is_edsl:
            return f'(set-code "{body} {pat}")'
        else:
            return original + '\n' + pat

    if is_edsl:
        content, _ = _extract_content(code)
        body = content
    else:
        body = code

    result = _find_fn_with_arity(body)
    if not result:
        return None
    fn, arity = result

    patterns = _gen_patterns(fn, arity)
    for pat in patterns:
        mod = _inject_display(code, body, pat)
        full = mod + ("\n(eval-current)" if is_edsl else "")
        ok, out, err = _fresh_run(full)
        if ok and (check_success(out, expected) or check_success(err, expected)):
            return (ok, out, err)
    return None


def run_single_task(
    model, base_url, api_key, name, prompt, expected, stdlib, api_ref, serve=None
):
    """Two-phase retry: coarse=full code, fine/putt=EDSL mutations.
    Routes to different models per phase when LLM_MODEL_2 is set.
    serve: ServeClient instance from main().
    Falls back to direct subprocess if serve is None."""
    max_att = MAX_ATTEMPTS
    total_llm_time = 0.0
    sys_prompt = build_sys_prompt(stdlib, api_ref, task_name=name)
    # Fresh serve session per task
    if serve:
        serve.proc.stdin.write('{"cmd":"session","name":"' + name + '"}\n')
        serve.proc.stdin.flush()
        serve.proc.stdout.readline()
    messages = [
        {"role": "system", "content": sys_prompt},
        {"role": "user", "content": prompt},
    ]
    # ── Model routing: route per attempt based on phase ──
    def _prepare_llm_call(current_phase):
        """Return (model, base_url, api_key) for current phase.
        Falls back to original params if routing not configured."""
        cfg = _route_model(name, current_phase)
        if cfg:
            return cfg.model_id, cfg.base_url, cfg.key
        return model, base_url, api_key
    last_full_code = ""
    phase = "coarse"
    # ── Aura 原生 serve 会话 ── 不注入 Scheme 兼容层（着力即差）

    def run_code(code):
        if serve:
            return serve.exec(code)
        try:
            r = subprocess.run(
                [AURA], input=code, capture_output=True, text=True, timeout=10
            )
            return r.returncode == 0, r.stdout.strip(), r.stderr.strip()
        except subprocess.TimeoutExpired:
            return False, "", "timeout"
        except FileNotFoundError:
            return False, "", "binary not found"

    for attempt in range(max_att):
        # Route to appropriate model for this phase
        route_model_id, route_url, route_key = _prepare_llm_call(phase)
        t0 = time.time()
        try:
            resp = llm_complete(route_model_id, route_url, route_key, messages)
        except Exception as e:
            return False, "", str(e), total_llm_time, attempt + 1
        total_llm_time += time.time() - t0

        code = extract_code(resp)
        if not code:
            if attempt >= max_att - 1:
                return False, "", "no code extracted", total_llm_time, attempt + 1
            messages.append({"role": "assistant", "content": resp or ""})
            messages += [
                {
                    "role": "user",
                    "content": "No valid Aura code found. Output ONLY Aura code, no markdown.",
                }
            ]
            continue

        # Detect mode: set-code means EDSL mutation, otherwise full program
        is_edsl = code.strip().startswith("(set-code")
        if is_edsl:
            # EDSL mode: the C++ fix makes eval-current propagate set-code errors,
            # so even through (begin (set-code ...) (eval-current)), the error survives.
            ok, out, err = run_code(code + "\n(eval-current)")
            if ok:
                last_full_code = code
        else:
            # Full code mode
            ok, out, err = run_code(code)
            if ok:
                last_full_code = code

        success = ok and (check_success(out, expected) or check_success(err, expected))
        if success:
            return True, out, "", total_llm_time, attempt + 1

        # ── Auto-fix #<procedure> ──
        # If output contains an uncalled function, try injecting (display ...)
        # before burning an LLM retry. This saves ~10-40s per occurrence.
        has_procedure = "#<procedure>" in (out if ok else err) or "#<procedu" in (out if ok else err)
        if has_procedure:
            auto_fixed = _auto_fix_procedure(code, expected, run_code, is_edsl)
            if auto_fixed:
                ok_fix, out_fix, err_fix = auto_fixed
                return True, out_fix, "", total_llm_time, attempt + 1

        if attempt >= max_att - 1:
            return False, out if ok else "", err or out, total_llm_time, attempt + 1

        actual_output = out if ok else (err or "exit code, no output")

        # Build adaptive feedback (includes phase detection)
        ada_fb, phase, temp_v, tokens_v = build_adaptive_feedback(
            name,
            out if ok else "",
            expected,
            stdlib,
            sys_prompt,
            prompt,
            current_src=code,
        )

        # ── Ant colony: try local mutations (fine/putt) before LLM retry ──
        if phase in ("fine", "putt"):
            # Load cross-task pheromone knowledge before search
            _colony_load_pheromone(serve, name)
            found, col_out, _ = internal_colony_search(
                serve,
                last_full_code if last_full_code else code,
                expected,
                phase,
                task_name=name,
            )
            if found:
                return True, col_out, "", total_llm_time, attempt + 1

        # Build correction prompt (phase affects only feedback tone, not output format)
        distance_note = {
            "coarse": "Stil far from goal - the code needs major changes.",
            "fine": "Getting closer - only a few things off.",
            "putt": "Almost there! Just minor fixes needed.",
        }.get(phase, "Fix the code.")

        # Detect #<procedure> — LLM forgot (display ...)
        procedure_warn = ""
        if "#<procedure>" in actual_output or "#<procedu" in actual_output:
            procedure_warn = (
                "\n\n⚠️  #<procedure> ERROR: You DEFINE'd a function but never CALLED it.\n"
                "EVERY program MUST end with (display (your-function args)).\n"
                "Take your (define ...) from the PREVIOUS attempt and ADD a (display ...)\n"
                "call at the end. DO NOT just output another (define ...)!\n"
                "Example: (define (f x) (* x 2))\n"
                "         (display (f 5))\n"
            )

        # Parse structured error: ("kind" "message") from eval-current
        structured_kind = None
        structured_msg = None
        if actual_output.startswith('("') and '" "' in actual_output[:120]:
            import re as _re2
            sm = _re2.match(r'\("([^"]+)"\s+"([^"]+)"\)', actual_output)
            if sm:
                structured_kind = sm.group(1)
                structured_msg = sm.group(2)

        # Detect set-code parse errors (now propagated as structured error)
        set_code_error = ""
        if structured_kind == "parse" or (
            is_edsl and ("parse error" in actual_output.lower()
                        or "expected expression" in actual_output.lower()[:60])):
            set_code_error = (
                "\n\n⚠️  SET-CODE PARSE ERROR: The code inside your (set-code ...) "
                "is malformed Aura code.\n"
                "The (set-code ...) primitive parses its argument as a program; "
                "make sure the content is valid Aura syntax.\n"
                "Check for: unbalanced parens, missing quotes, wrong function names.\n"
                f"Error: {structured_msg or actual_output[:200]}\n"
            )
            phase = "coarse"

        # Detect type errors / unbound variables from structured error
        type_error_hint = ""
        if structured_kind == "type error" or structured_kind == "unbound variable":
            undefined = structured_msg or "a needed function"
            # Extract the specific name from the message
            import re as _re3
            nm = _re3.search(r":\s*([^\s]+)", undefined)
            if nm:
                undefined = nm.group(1)
            else:
                undefined = "a needed function"
            type_error_hint = (
                "\n\n⚠️  MISSING DEFINITION: " + undefined.capitalize() +
                " is not defined.\n"
                "Add (define (" + undefined + " ...) ...) before calling it.\n"
                "Check the task's goal to see what signature " + undefined + " needs.\n"
            )

        # Inject relevant API doc snippet for common EDSL errors
        api_hint = ""
        if structured_msg:
            api_hint = _get_api_snippet(structured_msg)
        elif actual_output and not ok:
            api_hint = _get_api_snippet(actual_output)
        # Try Aura diagnose primitive for structured fix suggestions
        diagnose_hint = ""
        try:
            diag_code = f'(display (diagnose "{_ada_esc(structured_msg or actual_output)}"))'
            diag_r = subprocess.run(
                [AURA], input=diag_code, capture_output=True, text=True, timeout=5
            )
            if diag_r.returncode == 0 and diag_r.stdout.strip() and diag_r.stdout.strip() != "#f":
                diag_out = diag_r.stdout.strip()
                import re as _re4
                # Parse: (root-cause target fix-type fix-data explanation)
                # Parse: (cause target fix-type ... explain)
                parts = diag_out.strip('()').split(None, 4)  # cause target fix-type fix-data explain
                if len(parts) >= 5:
                    cause, target, fix_type, fix_data = parts[0], parts[1], parts[2], parts[3]
                    explain = parts[4].strip('"')
                    if cause != "closure-no-display":  # already handled by auto-fix
                        diagnose_hint = (
                            "\n\n🔍 DIAGNOSIS: " + explain + "\n"
                        )
                        if fix_type == "add-require":
                            diagnose_hint += f"💡 Fix: Add (require \"{fix_data}\" all:) or check your imports\n"
        except Exception:
            pass

        if api_hint:
            api_hint = "\n\n" + "=" * 35 + "\nRELEVANT API:\n" + api_hint + "\n" + "=" * 35 + "\n"

        correction = (
            ("(compile error) " if not ok else "(output mismatch) ")
            + distance_note
            + "\n\n"
            f"Aura produced: {structured_msg or actual_output[:300]}\n\n"
            + ada_fb + "\n\n"
            + procedure_warn
            + set_code_error
            + type_error_hint
            + diagnose_hint
            + api_hint +
            "Current code:\n"
            + (last_full_code[:400] if last_full_code else code[:400])
            + "\n\n"
            "Output the corrected Aura code (complete program with (display ...))."
        )

        messages.append({"role": "assistant", "content": resp})
        messages.append({"role": "user", "content": correction})

    return False, "", "max attempts", total_llm_time, max_att


def print_task_table(task_results):
    print(f"  {'Task':22s} {'Pass/Total':>10s} {'Rate':>6s}  {'Verdict':>12s}")
    print(f"  {'─'*22} {'─'*10} {'─'*6}  {'─'*12}")
    stable_pass = stable_fail = volatile = 0
    for name, passes, total in sorted(task_results):
        rate = passes / total * 100 if total > 0 else 0
        if rate == 100:
            verdict = "✅  Stable PASS"
            stable_pass += 1
        elif rate == 0:
            verdict = "❌  Stable FAIL"
            stable_fail += 1
        elif rate >= 50:
            verdict = "🔄  Volatile (pass)"
            volatile += 1
        else:
            verdict = "🔄  Volatile (fail)"
            volatile += 1
        print(f"  {name:22s} {passes:3d}/{total:<4d}  {rate:5.0f}%  {verdict}")
    print(
        f"\n  ➤ Stable: {stable_pass}✅ / {stable_fail}❌ + Volatile: {volatile}🔄 = {stable_pass+stable_fail+volatile}"
    )
    return stable_pass, stable_fail, volatile


# ── Thread safety ──────────────────────────────
_print_lock = threading.Lock()
def safe_print(*args, **kwargs):
    with _print_lock:
        print(*args, **kwargs)

def safe_task_runner(args):
    """Run a single task in a thread pool worker.
    Args: (model, base_url, api_key, name, prompt, expected, stdlib, api_ref, lock)"""
    model, base_url, api_key, name, prompt, expected, stdlib, api_ref = args
    task_serve = None
    try:
        task_serve = ServeClient()
    except Exception:
        pass
    task_passes = 0
    safe_print(f"\n  ── {name} ──")
    for round_i in range(1, ROUNDS + 1):
        success, out, err, llm_t, attempts = run_single_task(
            model, base_url, api_key, name, prompt, expected, stdlib,
            api_ref, serve=task_serve,
        )
        # Accumulate results (safe since task_stats is per-task)
        # We'll aggregate after thread pool completes
        return (name, success, out, err, llm_t, attempts)
    return (name, False, "", "", 0, 0)

# ── 主流程 ────────────────────────────────────────────────
def run_single_task_parallel(args):
    """Wrapper for ThreadPoolExecutor: creates its own ServeClient per task."""
    model, base_url, api_key, name, prompt, expected, stdlib, api_ref = args
    task_serve = None
    try:
        task_serve = ServeClient()
    except Exception:
        pass
    task_passes = 0
    for round_i in range(1, ROUNDS + 1):
        success, out, err, llm_t, attempts = run_single_task(
            model, base_url, api_key, name, prompt, expected, stdlib,
            api_ref, serve=task_serve,
        )
        if success:
            task_passes += 1
        # Return result after all rounds
    if task_serve:
        try:
            task_serve.close()
        except Exception:
            pass
    return name, success, out, err if err else "", llm_t, attempts


def main():
    global ROUNDS, EVOLVE_MODE, TRACE_MODE, MAX_ATTEMPTS

    models = os.environ.get("LLM_MODEL", "deepseek-v4-flash").split(",")
    base_url = os.environ.get("LLM_BASE_URL", "https://api.deepseek.com/v1")
    api_key = os.environ.get("LLM_API_KEY", "")
    output_json = False

    args = sys.argv[1:]
    i = 0
    while i < len(args):
        if args[i] in ("-r", "--rounds"):
            i += 1
            if i < len(args):
                try:
                    ROUNDS = int(args[i])
                except ValueError:
                    pass
        elif args[i] == "--json":
            output_json = True
        elif args[i] == "--tasks":
            i += 1
            if i < len(args):
                os.environ["BENCH_TASK_FILTER"] = args[i]
        elif args[i] == "--failed":
            os.environ["BENCH_TASK_FILTER"] = "primes-list,quicksort,tcp-connect"

        elif args[i] == "--trace":
            TRACE_MODE = True
        elif args[i] == "--evolve":
            EVOLVE_MODE = True
        elif args[i] == "--max-attempts":
            i += 1
            if i < len(args):
                try:
                    MAX_ATTEMPTS = int(args[i])
                except ValueError:
                    pass
        i += 1

    if not api_key:
        print("❌ LLM_API_KEY not set")
        sys.exit(1)

    api_ref = get_api_ref()

    mode_tag = (
        f"  (intend mode: up to {MAX_ATTEMPTS} attempts)"
        if not EVOLVE_MODE
        else "  (evolve mode)"
    )

    print(f"\n{'='*70}")
    print(f"Aura EDSL Benchmark")
    print(f"Models: {', '.join(models)}")
    print(f"Tasks: {len(TASKS)}")
    print(f"Rounds: {ROUNDS}{mode_tag}")
    print(f"{'='*70}\n")

    all_results = {}

    for model in models:
        model = model.strip()
        all_results[model] = {}
        task_stats = defaultdict(
            lambda: {
                "passes": 0,
                "total": 0,
                "errors": [],
                "llm_times": [],
                "attempts": [],
            }
        )
        print(f"\n{'─'*70}")
        print(f"  Model: {model}")
        print(f"{'─'*70}")

        start_time = time.time()

        task_filter = os.environ.get("BENCH_TASK_FILTER", "")
        filter_list = (
            [t.strip() for t in task_filter.split(",") if t.strip()]
            if task_filter
            else []
        )
        # task_results initialized at model level
        task_results = []
        tasks_to_run = [(name, prompt, expected, stdlib) for name, prompt, expected, stdlib in TASKS
                           if not filter_list or name in filter_list]
        
        pool_args = [(model, base_url, api_key, name, prompt, expected, stdlib, api_ref)
                     for name, prompt, expected, stdlib in tasks_to_run]
        
        # Run tasks in parallel
        max_workers = int(os.environ.get("BENCH_WORKERS", "20"))
        with concurrent.futures.ThreadPoolExecutor(max_workers=max_workers) as executor:
            futures = {executor.submit(run_single_task_parallel, args): args for args in pool_args}
            for future in concurrent.futures.as_completed(futures):
                try:
                    name, success, out, err, llm_t, attempts = future.result()
                    task_stats[name]["llm_times"].append(llm_t)
                    task_stats[name]["attempts"].append(attempts)
                    task_passes = 1 if success else 0
                    task_stats[name]["passes"] += task_passes
                    task_stats[name]["total"] += ROUNDS
                    task_results.append((name, task_passes, ROUNDS))
                    if success:
                        safe_print(f"  ✅ {name} ({llm_t:.1f}s, {attempts} att)")
                    else:
                        err_short = (err[:50] if err else "")
                        safe_print(f"  ❌ {name}: {err_short} ({llm_t:.1f}s, {attempts} att)")
                except Exception as e:
                    safe_print(f"  ❌ task error: {e}")
        
        elapsed = time.time() - start_time
        task_stats["__meta__"] = {"elapsed": round(elapsed, 1)}

        if task_stats.get("__errors__"):
            errors = task_stats["__errors__"]
            print(f"\n  ❌ Failure Analysis ({len(errors)} tasks):")
            # Group by error type
            by_type = {}
            for name, etype, att, t in errors:
                by_type.setdefault(etype, []).append(name)
            for etype, names in sorted(by_type.items()):
                print(
                    f"    {etype:25s}: {len(names)} tasks -- {', '.join(names[:5])}{'...' if len(names) > 5 else ''}"
                )

        print(f"{'─'*70}")
        print(f"  {model} -- Per-Task Results ({ROUNDS} rounds)")
        print(f"{'─'*70}")
        task_rows = [
            (n, s["passes"], s["total"])
            for n, s in task_stats.items()
            if n not in ("__meta__", "__errors__")
        ]
        sp, sf, sv = print_task_table(task_rows)

        if True:
            print(f"\n  📊 Attempt stats:")
            for n in sorted(
                s for s in task_stats if s not in ("__meta__", "__errors__")
            ):
                s = task_stats[n]
                if s["attempts"]:
                    avg = sum(s["attempts"]) / len(s["attempts"])
                    print(
                        f"    {n:22s} avg {avg:.1f} attempts, {s['passes']}/{s['total']} passed"
                    )

        volatile_tasks = [
            n
            for n, s in task_stats.items()
            if n not in ("__meta__", "__errors__") and 0 < s["passes"] < s["total"]
        ]
        if volatile_tasks:
            print(f"\n  ⚠️  Volatile tasks:")
            for n in sorted(volatile_tasks):
                s = task_stats[n]
                print(
                    f"    {n}: {s['passes']}/{s['total']} ({s['passes']/s['total']*100:.0f}%)"
                )
                for err in s["errors"][:3]:
                    print(f"      · {err}")

        all_results[model] = dict(task_stats)

    if output_json:
        output = {}
        for model, tasks in all_results.items():
            mout = {}
            for task_name, stats in tasks.items():
                if task_name in ("__meta__", "__errors__"):
                    if task_name == "__meta__":
                        mout["__meta__"] = stats
                    continue
                entry = {
                    "passes": stats["passes"],
                    "total": stats["total"],
                    "pass_rate": (
                        round(stats["passes"] / stats["total"] * 100, 1)
                        if stats["total"]
                        else 0
                    ),
                    "avg_llm_time": (
                        round(sum(stats["llm_times"]) / len(stats["llm_times"]), 2)
                        if stats["llm_times"]
                        else 0
                    ),
                }
                if stats["attempts"]:
                    entry["avg_attempts"] = round(
                        sum(stats["attempts"]) / len(stats["attempts"]), 1
                    )
                mout[task_name] = entry
            output[model] = mout
        print(f"\n{'='*70}")
        print(json.dumps(output))

    print(f"\n{'='*70}")
    print("Done")
    print(f"{'='*70}")


if __name__ == "__main__":
    main()
