# std/llm — chat and multimodal content

`std/llm` wraps an OpenAI-compatible chat-completions endpoint. The
endpoint, model and key come from `LLM_BASE_URL`, `LLM_MODEL` and the
`llm-api-key` entry of the secret store (#5417); `LLM_API_KEY` is a
deprecated compatibility fallback, not the recommended store. See
`docs/stdlib/secret.md` for provisioning and redaction. A sandboxed
process still needs `--grant net`.

## Text

```aura
(require "std/llm" all:)
(llm:chat "You are terse." "Say pong")
; → (hash "ok" #t "error" "" "content" "pong" "usage" #f)
```

The optional rest arguments are the same for every chat entry point: an
extra hash/alist of request fields (`max_tokens`, `reasoning_effort`, …)
and a timeout in seconds. `LLM_TIMEOUT` is the environment default.

## Multimodal content parts (#5418)

`llm:chat-vision` sends the user turn as an OpenAI-compatible content
array; `llm:chat` accepts the same parts list as its user argument.

```aura
(llm:chat-vision
  "You are a vision checker."
  (list (llm:text-part "What is the dominant color?")
        (llm:image-part (string-append "data:image/png;base64," png-base64)))
  (hash "max_tokens" 1024))
```

Part constructors:

| constructor | JSON |
|---|---|
| `(llm:text-part s)` | `{"type":"text","text":s}` |
| `(llm:image-part url)` | `{"type":"image_url","image_url":{"url":url}}` |

`url` may be a data URL or an `http(s)` URL. The image formats are PNG,
JPEG, GIF and WEBP. MiniMax documents a **10 MB per-image** ceiling
(JPEG/PNG/GIF/WEBP); a base64 data URL carries 4/3 of the binary size, so
the module refuses a data URL longer than 14,000,000 characters with
`image-too-large` before any bytes leave the process. A malformed or
unsupported part is `bad-content`, `unsupported-image-type` or
`unsupported-image-url`.

Text parts go through the same sanitizer and injection gate as a plain
prompt. Both run over the **concatenation of all text parts**, not per
part, so a marker split across two adjacent parts is seen and blocked like
the unsplit marker (#5443). The response parser is unchanged: an empty
`content` with a `reasoning_content` is reported as
`truncated/finish=<reason>`, so give
the model enough `max_tokens` to finish its thinking and its answer.

`llm:chat-request` is the pure request builder (no network); it is what
the tests use to check the JSON shape.

## Token usage (#5481)

Every parse result carries a `usage` key: a nested hash with the
provider's numbers, or `#f` when the response did not include them. The
core numbers are `prompt_tokens`, `completion_tokens` and
`total_tokens`; `reasoning_tokens` and `cached_tokens` appear only when
the provider sent them. No value is estimated. A truncated answer keeps
its usage too, so the caller can still charge the call.

```aura
(let ((u (hash-ref (llm:chat "You are terse." "Say pong") "usage" #f)))
  (if (hash? u)
      (hash-ref u "total_tokens")
      0))
```

`llm:chat-vision` returns the same shape. `llm:call-stats` stays a count
only; token accounting is per call from `usage`.

## Rate limit and statistics

`llm:rate-limit-set!` bounds calls per minute, and `llm:call-stats`
counts successes, transport failures, rate limits, injection blocks,
sanitized prompts and parse outcomes. `llm:parse-response` is the parser
behind both chat entry points.
