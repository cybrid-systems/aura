;;; aura-lang-lsp-test.el --- ERT: aura-mode through lsp-mode and eglot  -*- lexical-binding: t; -*-

;;; Commentary:

;; End-to-end tests for the two Emacs LSP clients against a real
;; "aura lsp".  The client under test comes from AURA_LANG_EMACS_CLIENT
;; (eglot or lsp-mode); test/run-ert.sh runs the same file once per
;; client, which is what CI does.
;;
;; These tests use the public client entry points where they exist:
;;
;;   - eglot: eglot--connect + eglot--maybe-activate-editing-mode (batch
;;     has no idle loop to run eglot-ensure), then Flymake for the
;;     diagnostics the user sees.
;;   - lsp-mode: lsp with lsp-auto-guess-root (aura.toml is a project
;;     root through aura-lang-project-root), lsp-diagnostics for the
;;     published set, and xref for jumps.
;;
;; The Aura binary comes from AURA_LANG_TEST_BIN, aura-lang-executable,
;; or PATH.  Without one the tests skip; the driver script fails the CI
;; job when the binary is missing instead.
;;
;; A2 and A6 are in this tree, so the comment-definition case is a normal
;; expected pass and aura-lang-lsp-formatting-keeps-comments drives the
;; client's format-buffer, asserting Chinese comments survive verbatim and
;; a second format is a no-op.

;;; Code:

(require 'ert)
(require 'flymake)
(require 'xref)
(require 'aura-lang)

(defconst aura-lang-lsp-test-timeout 30
  "Seconds to wait for a client or a diagnostic.")

(defun aura-lang-lsp-test--client ()
  "Return the client under test, eglot or lsp-mode."
  (intern (or (getenv "AURA_LANG_EMACS_CLIENT") "eglot")))

(defun aura-lang-lsp-test--binary ()
  "Return the Aura executable for the tests, or nil."
  (let ((candidate (or (getenv "AURA_LANG_TEST_BIN")
                       (ignore-errors (aura-lang-executable))
                       (executable-find "aura"))))
    (and candidate (file-executable-p candidate) candidate)))

(defun aura-lang-lsp-test--wait (predicate &optional seconds)
  "Wait until PREDICATE returns non-nil, or SECONDS elapse."
  (let ((deadline (+ (float-time) (or seconds aura-lang-lsp-test-timeout))))
    (while (and (< (float-time) deadline) (not (funcall predicate)))
      (accept-process-output nil 0.2))
    (funcall predicate)))

(defun aura-lang-lsp-test--write-file (root name text)
  "Write TEXT to NAME under ROOT, creating parent directories."
  (let ((file (expand-file-name name root)))
    (make-directory (file-name-directory file) t)
    (with-temp-file file (insert text))))

(defun aura-lang-lsp-test--project (files &optional prefix)
  "Create a temporary project with FILES (an alist of name . text)."
  (let* ((root (make-temp-file (or prefix "aura-lsp-test-") t))
         (default-directory root))
    (aura-lang-lsp-test--write-file root "aura.toml"
                                    "[package]\nname = \"lsp-test\"\n")
    (dolist (entry files)
      (aura-lang-lsp-test--write-file root (car entry) (cdr entry)))
    root))

(defun aura-lang-lsp-test--connect (buffer)
  "Connect BUFFER through the client under test and wait for readiness."
  (with-current-buffer buffer
    (flymake-mode 1)
    (pcase (aura-lang-lsp-test--client)
      ('eglot
       (require 'eglot)
       ;; eglot-ensure needs the idle loop; batch connects directly.
       (apply #'eglot--connect (eglot--guess-contact))
       (should (aura-lang-lsp-test--wait (lambda () (eglot-current-server))))
       ;; aura exits once during eglot's first handshake and eglot
       ;; reconnects; let that settle before binding didOpen/Flymake to
       ;; the surviving server, or the edit test loses its backend.
       (accept-process-output nil 1.0)
       (when (eglot-current-server)
         (eglot--maybe-activate-editing-mode)
         ;; eglot 31 tracks a documented version; older eglot (Emacs
         ;; 29.3) has no such variable and is left alone.
         (when (and (boundp 'eglot--docver) (cl-minusp eglot--docver))
           (eglot--signal-textDocument/didOpen))
         ;; The Flymake backend is only installed once managed; without
         ;; this start no push ever reaches flymake-diagnostics.
         (flymake-mode 1)
         (flymake-start))
       (should (eglot-current-server)))
      ('lsp-mode
       (require 'lsp-mode)
       ;; Ubuntu's elpa-lsp-mode is loaded from load-path in batch, so its
       ;; autoloads file is not registered and the configure hook calls
       ;; optional modeline commands that are not defined. Stub them: these
       ;; are UI extras, and a void-function here aborts the workspace
       ;; before any publishDiagnostics reaches the client.
       (dolist (cmd '(lsp-modeline-workspace-status-mode
                      lsp-modeline-diagnostics-mode
                      lsp-modeline-code-actions-mode
                      lsp-headerline-breadcrumb-mode))
         (unless (fboundp cmd)
           (defalias cmd #'ignore)))
       (setq lsp-auto-guess-root t
             lsp-restart 'ignore
             lsp-lens-enable nil
             lsp-enable-snippet nil)
       (setq-local lsp-diagnostics-provider :flymake)
       (lsp)
       (should (aura-lang-lsp-test--wait
                (lambda ()
                  (and (lsp-workspaces)
                       (eq (lsp--workspace-status (car (lsp-workspaces)))
                           'initialized)))))
       ;; didOpen follows the initialize callback; give it one turn.
       (accept-process-output nil 0.5))
      (other (error "Unknown AURA_LANG_EMACS_CLIENT: %S" other)))))

(defun aura-lang-lsp-test--eglot-diagnostics ()
  "Return (MESSAGE . TEXT) for the buffer's Flymake diagnostics."
  (mapcar (lambda (diagnostic)
            (cons (flymake-diagnostic-text diagnostic)
                  (buffer-substring-no-properties
                   (flymake-diagnostic-beg diagnostic)
                   (flymake-diagnostic-end diagnostic))))
          (flymake-diagnostics)))

(defun aura-lang-lsp-test--lsp-mode-diagnostics ()
  "Return (MESSAGE . TEXT) for lsp-mode's published diagnostics."
  (when-let* ((workspace (car (lsp-workspaces)))
              (file (buffer-file-name)))
    (let ((table (lsp-diagnostics workspace))
          (uri (lsp--path-to-uri file)))
      (cl-loop for key being the hash-keys of table using (hash-value items)
               when (or (equal key file) (equal key uri))
               append (cl-loop for item in (cond ((vectorp items)
                                                   (append items nil))
                                                  ((listp items) items)
                                                  (t (list items)))
                               collect (cons (gethash "message" item)
                                             (ignore-errors
                                               (lsp--range-text
                                                (gethash "range" item)))))))))

(defun aura-lang-lsp-test--diagnostics ()
  "Return the buffer's (MESSAGE . TEXT) diagnostics for this client."
  (pcase (aura-lang-lsp-test--client)
    ('eglot (aura-lang-lsp-test--eglot-diagnostics))
    ('lsp-mode (aura-lang-lsp-test--lsp-mode-diagnostics))))

(defun aura-lang-lsp-test--wait-diagnostic (regexp &optional seconds)
  "Wait for a diagnostic whose message matches REGEXP; return it.
A nil result after SECONDS means none arrived; a negative assertion
should pass a short SECONDS instead of the full client timeout."
  (aura-lang-lsp-test--wait
   (lambda ()
     (seq-find (lambda (entry) (string-match-p regexp (car entry)))
               (aura-lang-lsp-test--diagnostics)))
   seconds))

(defun aura-lang-lsp-test--flush-changes ()
  "Send the pending didChange that the client's idle timer would send.
Batch Emacs never becomes idle, so both clients' idle timers stay
armed; this calls the same sender the timer calls."
  (pcase (aura-lang-lsp-test--client)
    ;; #5359: a pull client has no push to wait for, so start the
    ;; diagnostics backend the way the idle loop would. A push-only client
    ;; (eglot before pull support) must be left alone: its flymake set
    ;; arrives from the server's push, and an extra backend run there
    ;; swallows the report.
    ('eglot
     (eglot--signal-textDocument/didChange)
     (when (and (fboundp 'eglot-server-capable)
                (eglot-server-capable :diagnosticProvider))
       (flymake-start)))
    ('lsp-mode (lsp--flush-delayed-changes))))

(defun aura-lang-lsp-test--formatting-capability ()
  "Return the server's documentFormattingProvider value."
  (pcase (aura-lang-lsp-test--client)
    ('eglot
     (or (eglot-server-capable :documentFormattingProvider) :absent))
    ('lsp-mode
     (let ((caps (lsp--workspace-server-capabilities (car (lsp-workspaces)))))
       (or (gethash "documentFormattingProvider" caps) :absent)))))

(defun aura-lang-lsp-test--jump (context name)
  "Jump to the definition of NAME, which follows CONTEXT.
Return (FILE LINE LINE-TEXT)."
  (goto-char (point-min))
  (search-forward context)
  (goto-char (+ (- (point) (length context))
                (string-match (regexp-quote name) context)))
  (let ((identifier (if (eq (aura-lang-lsp-test--client) 'lsp-mode)
                        (propertize name 'identifier-at-point t)
                      name)))
    (xref-find-definitions identifier))
  (list buffer-file-name
        (line-number-at-pos)
        (string-trim
         (buffer-substring-no-properties (line-beginning-position)
                                         (line-end-position)))))

(defun aura-lang-lsp-test--shutdown ()
  "Stop the client under test and its server process."
  (pcase (aura-lang-lsp-test--client)
    ('eglot
     (when-let* ((server (eglot-current-server)))
       (ignore-errors (eglot-shutdown server 2))
       (when-let* ((process (jsonrpc--process server)))
         (delete-process process))))
    ('lsp-mode
     (dolist (workspace (lsp-workspaces))
       (when-let* ((process (lsp--workspace-proc workspace)))
         (delete-process process))))))

(defun aura-lang-lsp-test--with-project (files prefix function)
  "Create a project with FILES and PREFIX, connect, call FUNCTION.
FUNCTION runs in the connected main.aura buffer; the server is shut
down and the project removed afterwards."
  (let ((root (aura-lang-lsp-test--project files prefix)))
    (let ((buffer (find-file-noselect (expand-file-name "main.aura" root))))
      (unwind-protect
          (with-current-buffer buffer
            (aura-lang-lsp-test--connect buffer)
            (funcall function))
        (with-current-buffer buffer
          (ignore-errors (aura-lang-lsp-test--shutdown)))
        (when (buffer-live-p buffer) (kill-buffer buffer))
        (delete-directory root t)))))

(defun aura-lang-lsp-test--ready ()
  "Skip the test unless a client and an Aura binary are available."
  ;; skip-unless only expands inside ert-deftest bodies; this helper is a
  ;; plain function, so it calls ert-skip directly.
  (unless (memq (aura-lang-lsp-test--client) '(eglot lsp-mode))
    (ert-skip "unknown AURA_LANG_EMACS_CLIENT"))
  (let ((binary (aura-lang-lsp-test--binary)))
    (unless binary
      (ert-skip "aura binary not found"))
    ;; AURA_LANG_TEST_BIN must win over an installed aura on PATH; the
    ;; CI job tests the binary built from this commit.
    (setq aura-lang-executable binary)
    (message "aura-lang ERT binary: %s (%s)" binary (string-trim
                                                     (shell-command-to-string
                                                      (concat (shell-quote-argument binary)
                                                              " --version"))))))

(ert-deftest aura-lang-lsp-client-loads ()
  "The selected client is installed and can start a server."
  (pcase (aura-lang-lsp-test--client)
    ('eglot
     (require 'eglot)
     (should (fboundp 'eglot-ensure))
     (should (fboundp 'eglot--connect)))
    ('lsp-mode
     (require 'lsp-mode)
     (should (fboundp 'lsp))
     (should (fboundp 'lsp-request)))))

(ert-deftest aura-lang-lsp-diagnostics-on-open ()
  "Opening an .aura file immediately publishes its diagnostics."
  (aura-lang-lsp-test--ready)
  (aura-lang-lsp-test--with-project
      '(("main.aura" . "(define (f x)\n  (memq x 1))\n")) nil
    (lambda ()
      ;; #5364: the message carries the CLI wording, so lsp-mode and
      ;; flycheck (which show only the message) stay readable.  The text
      ;; under the diagnostic is still the bare symbol.
      (let ((diagnostic (aura-lang-lsp-test--wait-diagnostic "memq$")))
        (should diagnostic)
        (should (equal (cdr diagnostic) "memq"))
        (should (string-match-p "unbound variable: memq" (car diagnostic)))))))

(ert-deftest aura-lang-lsp-type-warning-does-not-mask-unbound ()
  "A type warning does not hide a later unbound name (#5346).
Both clients must surface memq, not only the earlier type warning."
  (aura-lang-lsp-test--ready)
  (aura-lang-lsp-test--with-project
      '(("main.aura" . "(define (g y) (+ y \"s\"))\n(define (h x) (memq x (list 1 2)))\n")) nil
    (lambda ()
      (let ((unbound (aura-lang-lsp-test--wait-diagnostic "memq$"))
            (warning (aura-lang-lsp-test--wait-diagnostic "expected Int")))
        (should unbound)
        (should (equal (cdr unbound) "memq"))
        (should (string-match-p "unbound variable: memq" (car unbound)))
        (should warning)))))

(ert-deftest aura-lang-lsp-diagnostics-unsaved-edit ()
  "An edit that is not saved is analyzed after the debounce (A1)."
  (aura-lang-lsp-test--ready)
  (aura-lang-lsp-test--with-project
      '(("main.aura" . "(define (f x)\n  x)\n")) nil
    (lambda ()
      (let ((file buffer-file-name))
        (should-not (aura-lang-lsp-test--wait-diagnostic "oops$" 2))
        (goto-char (point-max))
        (insert "(display oops)\n")
        (set-buffer-modified-p t)
        (aura-lang-lsp-test--flush-changes)
        (let ((diagnostic (aura-lang-lsp-test--wait-diagnostic "oops$")))
          (should diagnostic)
          (should (buffer-modified-p))
          ;; The disk file still has no oops: the server used didChange,
          ;; not a didSave/file read.
          (should-not (with-temp-buffer
                        (insert-file-contents file)
                        (string-match-p "oops" (buffer-string)))))))))

(ert-deftest aura-lang-lsp-comment-definition-does-not-bind ()
  "A definition inside a comment does not bind the name (A2)."
  ;; A2 (reader-based symbol layer) is in this tree, so this is a normal
  ;; expected pass: the comment must not define ghost and the unbound use
  ;; must be reported.
  (aura-lang-lsp-test--ready)
  (aura-lang-lsp-test--with-project
      '(("main.aura" . "; (define ghost 1)\n(display ghost)\n")) nil
    (lambda ()
      ;; Five seconds is many debounce windows; the analysis itself is
      ;; immediate on didOpen, so a nil result would be a real regression.
      (should (aura-lang-lsp-test--wait-diagnostic "ghost$" 5)))))

(ert-deftest aura-lang-lsp-path-with-plus-and-at ()
  "A project path containing + and @ still analyses the buffer (A1)."
  (aura-lang-lsp-test--ready)
  (aura-lang-lsp-test--with-project
      '(("main.aura" . "(define (f x)\n  (memq x 1))\n"))
      "aura-lsp+smoke@"
    (lambda ()
      (should (string-match-p "[+@]" (buffer-file-name)))
      (should (aura-lang-lsp-test--wait-diagnostic "memq$")))))

(ert-deftest aura-lang-lsp-definition-same-file ()
  "Definition of a local function jumps inside the same file."
  (aura-lang-lsp-test--ready)
  (aura-lang-lsp-test--with-project
      '(("main.aura" . "(define (same x) x)\n(display (same 2))\n")) nil
    (lambda ()
      (let ((target (aura-lang-lsp-test--jump "(display (same" "same")))
        (should (equal (file-truename (nth 0 target))
                       (file-truename buffer-file-name)))
        (should (equal (nth 1 target) 1))
        (should (string-prefix-p "(define (same" (nth 2 target)))))))

(ert-deftest aura-lang-lsp-definition-cross-require ()
  "Definition in a required local module is found."
  (aura-lang-lsp-test--ready)
  (aura-lang-lsp-test--with-project
      '(("main.aura" . "(require \"lib/util.aura\" all:)\n(display (helper 1))\n")
        ("lib/util.aura" . "(define (helper x)\n  (+ x 1))\n")) nil
    (lambda ()
      (let ((target (aura-lang-lsp-test--jump "(display (helper" "helper")))
        (should (string-suffix-p "lib/util.aura" (nth 0 target)))
        (should (equal (nth 1 target) 1))
        (should (string-prefix-p "(define (helper" (nth 2 target)))))))

(ert-deftest aura-lang-lsp-definition-std ()
  "Definition in a std module is found through module search."
  (aura-lang-lsp-test--ready)
  (aura-lang-lsp-test--with-project
      '(("main.aura" . "(require \"std/list\" all:)\n(display (foldr + 0 (list 1 2)))\n")) nil
    (lambda ()
      (let ((target (aura-lang-lsp-test--jump "(display (foldr" "foldr")))
        (should (string-suffix-p "std/list.aura" (nth 0 target)))
        (should (string-prefix-p "(define (foldr" (nth 2 target)))))))

(ert-deftest aura-lang-lsp-formatting-keeps-comments ()
  "A6: format-buffer through the client keeps Chinese comments verbatim."
  (aura-lang-lsp-test--ready)
  (aura-lang-lsp-test--with-project
      '(("main.aura" . "; 中文注释：保留\n(define (f x)\n(+ x 1)) ; 行尾\n"))
    nil
    (lambda ()
      (should (aura-lang-lsp-test--formatting-capability))
      (let ((format (lambda ()
                      (pcase (aura-lang-lsp-test--client)
                        ('eglot (eglot-format-buffer))
                        ('lsp-mode (lsp-format-buffer))))))
        (funcall format)
        (should (string-match-p "中文注释：保留" (buffer-string)))
        (should (string-match-p "行尾" (buffer-string)))
        (should (string-match-p "(define (f x)" (buffer-string)))
        (should-not (string-match-p "(define f (lambda" (buffer-string)))
        ;; idempotent: a second format is a no-op
        (let ((once (buffer-string)))
          (funcall format)
          (should (equal once (buffer-string))))))))

(ert-deftest aura-lang-lsp-chinese-columns ()
  "A diagnostic after Chinese text covers exactly the bad symbol."
  (aura-lang-lsp-test--ready)
  (aura-lang-lsp-test--with-project
      '(("main.aura" . "(define s \"漢字\") (display oops)\n")) nil
    (lambda ()
      (let ((diagnostic (aura-lang-lsp-test--wait-diagnostic "oops$")))
        (should diagnostic)
        ;; Both clients negotiate a real encoding (utf-8 with today's
        ;; eglot/lsp-mode).  A byte offset copied without conversion would
        ;; underline part of the string instead of oops.
        (should (equal (cdr diagnostic) "oops"))
        (should (string-match-p "unbound variable: oops" (car diagnostic)))))))

(ert-deftest aura-lang-lsp-diagnostics-are-not-duplicated ()
  "A pull-capable client sees each diagnostic once (#5359).
eglot 1.24 and later report both the pushed and the pulled set, so the
server must send one channel when the client declares
textDocument.diagnostic; lsp-mode does not declare it and keeps the push."
  (aura-lang-lsp-test--ready)
  (aura-lang-lsp-test--with-project
      '(("main.aura" .
         "(define (g y) (+ y \"s\"))\n(define (h x) (memq x (list 1 2)))\n(define (k) (undefined-fn 1))\n"))
      nil
    (lambda ()
      (should (aura-lang-lsp-test--wait-diagnostic "undefined-fn$"))
      ;; Let a second publication (or a late pull) arrive before counting.
      (accept-process-output nil 1.0)
      (let ((diagnostics (aura-lang-lsp-test--diagnostics))
            (seen nil)
            (duplicates nil))
        (dolist (entry diagnostics)
          (if (member entry seen)
              (push entry duplicates)
            (push entry seen)))
        (should diagnostics)
        (should-not duplicates)))))

(defun aura-lang-lsp-test--diagnostic-severity (regexp)
  "Return the lsp-mode severity of the first diagnostic matching REGEXP."
  (when-let* ((workspace (car (lsp-workspaces)))
              (file (buffer-file-name)))
    (let ((table (lsp-diagnostics workspace))
          (uri (lsp--path-to-uri file)))
      (cl-loop for key being the hash-keys of table using (hash-value items)
               when (or (equal key file) (equal key uri))
               thereis (cl-loop for item in (cond ((vectorp items)
                                                   (append items nil))
                                                  ((listp items) items)
                                                  (t (list items)))
                                when (string-match-p regexp (gethash "message" item))
                                return (gethash "severity" item))))))

;; #5358: lsp-mode sends the aura section it is configured with.
(ert-deftest aura-lang-lsp-custom-settings-are-sent ()
  "lsp-mode sends the aura section, so aura.typeDiagnostics works (#5358).
The client used to register no :initialized-fn and no
:initialization-options, so values from lsp-register-custom-settings
never reached the server."
  (skip-unless (eq (aura-lang-lsp-test--client) 'lsp-mode))
  (aura-lang-lsp-test--ready)
  (require 'lsp-mode)
  (unwind-protect
      (progn
        (lsp-register-custom-settings '(("aura.typeDiagnostics" "off")))
        (aura-lang-lsp-test--with-project
            '(("main.aura" .
               "(define (g y) (+ y \"s\"))\n(define (h x) (memq x (list 1 2)))\n"))
            nil
          (lambda ()
            ;; Pull on demand: batch Emacs has no idle loop to trigger the
            ;; client's own refresh, and the point is what the server holds.
            (let* ((uri (lsp--path-to-uri buffer-file-name))
                   (pull (lambda ()
                           (lsp-request
                            "textDocument/diagnostic"
                            (list :textDocument (list :uri uri)))))
                   (items (lambda (result)
                            (append (and result (gethash "items" result)) nil))))
              (let ((first (funcall items (funcall pull))))
                (should (seq-find (lambda (item)
                                    (string-match-p "unbound variable: memq"
                                                    (gethash "message" item)))
                                  first))
                (should-not (seq-find (lambda (item)
                                        (string-match-p "expected Int"
                                                        (gethash "message" item)))
                                      first)))
              ;; Promote the same key: didChangeConfiguration must reanalyze.
              (lsp-register-custom-settings '(("aura.typeDiagnostics" "error")))
              (with-lsp-workspace (car (lsp-workspaces))
                (lsp--set-configuration (lsp-configuration-section "aura")))
              (let* ((second (funcall items (funcall pull)))
                     (promoted (seq-find
                                (lambda (item)
                                  (string-match-p "expected Int"
                                                  (gethash "message" item)))
                                second)))
                (should promoted)
                (should (equal 1 (gethash "severity" promoted))))))))
    ;; Leave the default registered for the tests that follow.
    (lsp-register-custom-settings '(("aura.typeDiagnostics" "warning")))))

(provide 'aura-lang-lsp-test)
;;; aura-lang-lsp-test.el ends here
