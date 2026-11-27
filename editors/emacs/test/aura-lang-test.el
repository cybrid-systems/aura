;;; aura-lang-test.el --- ERT tests for aura-lang  -*- lexical-binding: t; -*-

;;; Commentary:

;; Run from editors/emacs:
;;
;;   emacs -Q --batch -L . -l test/aura-lang-test.el \
;;         -f ert-run-tests-batch-and-exit
;;
;; Tests that need the Aura binary are skipped when it is missing.
;; This file is not part of the Aura test homes under tests/.

;;; Code:

(require 'ert)
(require 'imenu)
(require 'aura-lang)

(defun aura-lang-test--face-at (needle offset)
  "Return the face at NEEDLE plus OFFSET in the current buffer."
  (goto-char (point-min))
  (search-forward needle)
  (get-text-property (+ (match-beginning 0) offset) 'face))

(defun aura-lang-test--buffer (text)
  "Return a buffer with TEXT in aura-mode."
  (let ((buffer (generate-new-buffer " *aura-test*")))
    (with-current-buffer buffer
      (insert text)
      (setq buffer-file-name (make-temp-file "aura-test" nil ".aura"))
      (aura-mode)
      buffer)))

(defun aura-lang-test--in-buffer (text function)
  "Call FUNCTION in a temporary aura-mode buffer holding TEXT."
  (let ((buffer (aura-lang-test--buffer text)))
    (unwind-protect
        (with-current-buffer buffer (funcall function))
      (kill-buffer buffer))))

(ert-deftest aura-lang-loads ()
  "The package loads and the mode is available."
  (should (featurep 'aura-lang))
  (should (fboundp 'aura-mode))
  (should (fboundp 'run-aura))
  (should (fboundp 'inferior-aura-mode)))

(ert-deftest aura-lang-grammar-is-the-source ()
  "Keywords come from the TextMate grammar, not a second hand copy."
  (skip-unless (aura-lang--grammar-file))
  (let* ((report (aura-lang-check-grammar))
         (grammar-keywords (alist-get :grammar-keywords report))
         (grammar-defines (alist-get :grammar-defines report)))
    (should grammar-keywords)
    (should (equal (alist-get :missing-in-mode report) nil))
    (dolist (kw grammar-keywords)
      (should (member kw aura-lang-keywords)))
    (dolist (form grammar-defines)
      (should (member form aura-lang-define-forms)))
    (should (equal (alist-get :missing-defines-in-mode report) nil))
    (should (equal (seq-difference aura-lang--extra-define-forms
                                   (alist-get :extra-in-mode report))
                   nil))))

(ert-deftest aura-lang-mode-activates-for-aura-files ()
  "Opening a .aura file selects aura-mode, derived from lisp-data-mode."
  (with-temp-buffer
    (setq buffer-file-name (make-temp-file "mode" nil ".aura"))
    (insert "(define x 1)\n")
    (set-auto-mode)
    (should (eq major-mode 'aura-mode))
    (should (derived-mode-p 'lisp-data-mode))
    (should (equal comment-start ";"))
    (should imenu-generic-expression)
    (delete-file buffer-file-name)))

(ert-deftest aura-lang-comments ()
  "Line and block comments work; #; does not start a comment."
  (aura-lang-test--in-buffer
   "x ; line\n#| block\nstill |# y\n#; ignored\n"
   (lambda ()
     (goto-char (point-min))
     (search-forward "line")
     (should (nth 4 (syntax-ppss)))
     (goto-char (point-min))
     (search-forward "still")
     (should (nth 4 (syntax-ppss)))
     (search-forward "y")
     (should-not (nth 4 (syntax-ppss)))
     (goto-char (point-min))
     (search-forward "#;")
     (should-not (nth 4 (syntax-ppss))))))

(ert-deftest aura-lang-comment-region ()
  "Commenting and uncommenting the region uses ; as the comment start."
  (let ((transient-mark-mode t))
    (aura-lang-test--in-buffer
     "(define x 1)\n(display x)\n"
     (lambda ()
       (goto-char (point-min))
       (set-mark (point-max))
       (activate-mark)
       (comment-dwim nil)
       (should (equal (buffer-string) ";; (define x 1)\n;; (display x)\n"))
       (goto-char (point-min))
       (set-mark (point-max))
       (activate-mark)
       (comment-dwim nil)
       (should (equal (buffer-string) "(define x 1)\n(display x)\n"))))))

(ert-deftest aura-lang-font-lock-faces ()
  "Keywords, definitions, literals and namespaces are highlighted."
  (aura-lang-test--in-buffer
   "(define (f x)\n  (+ x 1))\n(define-hygienic-macro (m y) y)\n(when (= x 1)\n  (display \"hi\" :kw 42 #\\a #t mutate:foo))\n; note\n"
   (lambda ()
     (font-lock-ensure)
     (should (eq (aura-lang-test--face-at "(define" 1) 'font-lock-keyword-face))
     (should (eq (aura-lang-test--face-at "(f x" 1) 'font-lock-function-name-face))
     (should (eq (aura-lang-test--face-at "(define-hygienic-macro" 1)
                 'font-lock-keyword-face))
     (should (eq (aura-lang-test--face-at "(m y" 1) 'font-lock-function-name-face))
     (should (eq (aura-lang-test--face-at "(when" 1) 'font-lock-keyword-face))
     (should (eq (aura-lang-test--face-at "\"hi\"" 0) 'font-lock-string-face))
     (should (eq (aura-lang-test--face-at ":kw" 0) 'font-lock-constant-face))
     (should (eq (aura-lang-test--face-at "42" 0) 'font-lock-constant-face))
     (should (eq (aura-lang-test--face-at "#\\a" 0) 'font-lock-constant-face))
     (should (eq (aura-lang-test--face-at "#t" 0) 'font-lock-constant-face))
     (should (eq (aura-lang-test--face-at "mutate:foo" 0) 'font-lock-builtin-face))
     (should (eq (aura-lang-test--face-at "note" 0) 'font-lock-comment-face)))))

(ert-deftest aura-lang-indent ()
  "Indent-region puts bodies at two columns, matching the formatter."
  (aura-lang-test--in-buffer
   "(define (f x)\n(+ x 1))\n(let ((a 1))\n(when a\n(display \"hi\")))\n(cond ((= a 1)\n(display 1))\n(else\n(display 2)))\n"
   (lambda ()
     (indent-region (point-min) (point-max))
     (should (equal (buffer-string)
                    (concat "(define (f x)\n"
                            "  (+ x 1))\n"
                            "(let ((a 1))\n"
                            "  (when a\n"
                            "    (display \"hi\")))\n"
                            "(cond ((= a 1)\n"
                            "       (display 1))\n"
                            "      (else\n"
                            "       (display 2)))\n"))))))

(ert-deftest aura-lang-imenu ()
  "Imenu lists every definition without an LSP server."
  (aura-lang-test--in-buffer
   "(define (f x) x)\n(define-hygienic-macro (m y) y)\n(define-type point (x y))\n"
   (lambda ()
     (let ((names (mapcar (lambda (entry)
                            (if (consp entry) (car entry) entry))
                          (imenu--make-index-alist))))
       (should (member "f" names))
       (should (member "m" names))
       (should (member "point" names))))))

(ert-deftest aura-lang-autoloads-load-before-project ()
  "Generated autoloads load in emacs -Q before project.el exists.
`project-find-functions' is not bound until project.el is loaded.  After
that load, `aura-lang-project-root' is on the list."
  (let* ((root (make-temp-file "aura-loaddefs" t))
         (src (expand-file-name "pkg" root))
         (out (expand-file-name "aura-lang-autoloads.el" root))
         (lib (or (locate-library "aura-lang.el" t)
                  (let ((compiled (locate-library "aura-lang")))
                    (and compiled
                         (expand-file-name
                          "aura-lang.el"
                          (file-name-directory compiled))))))
         (emacs (expand-file-name invocation-name invocation-directory)))
    (unwind-protect
        (progn
          (should (and lib (file-readable-p lib)))
          (should (string-suffix-p ".el" lib))
          (make-directory src)
          (copy-file lib (expand-file-name "aura-lang.el" src) t)
          (require 'loaddefs-gen)
          (loaddefs-generate src out)
          (should (file-readable-p out))
          (with-temp-buffer
            (insert-file-contents out)
            (goto-char (point-min))
            (should (search-forward "(with-eval-after-load 'project" nil t))
            ;; The registration must not run at top level.  The deferred
            ;; form is indented inside `with-eval-after-load'.
            (goto-char (point-min))
            (should-not (re-search-forward
                         "^\\((add-to-list 'project-find-functions\\)" nil t)))
          (with-temp-buffer
            (let ((status
                   (call-process
                    emacs nil (list t nil) nil
                    "-Q" "--batch"
                    "--eval" (format "(load %S)" out)
                    "--eval" "(require 'project)"
                    "--eval"
                    (concat "(unless (and (boundp 'project-find-functions)"
                            " (memq 'aura-lang-project-root project-find-functions))"
                            " (error \"missing: %S\" project-find-functions))"))))
              (unless (eq 0 status)
                (error "emacs -Q autoload check failed (%s):\n%s"
                       status (buffer-string))))))
      (delete-directory root t))))

(ert-deftest aura-lang-project-root ()
  "aura.toml marks the project root for project-current."
  (let* ((root (make-temp-file "aura-project" t))
         (sub (expand-file-name "src/lib" root)))
    (unwind-protect
        (progn
          (make-directory sub t)
          (with-temp-file (expand-file-name "aura.toml" root)
            (insert "[package]\nname = \"demo\"\n"))
          (let ((project (project-current nil sub)))
            (should project)
            (should (equal (file-name-as-directory (project-root project))
                           (file-name-as-directory root)))))
      (delete-directory root t))))

(ert-deftest aura-lang-executable-override ()
  "aura-lang-executable overrides discovery."
  (let ((script (make-temp-file "aura-fake" nil ".sh")))
    (unwind-protect
        (progn
          (with-temp-file script
            (insert "#!/bin/sh\necho 'aura 0.2.6'\n"))
          (set-file-modes script #o755)
          (let ((aura-lang-executable script))
            (should (equal (aura-lang-executable) script))
            (should (equal (aura-lang-lsp-command) (list script "lsp")))
            (should (eq (plist-get (aura-lang-version) :kind) 'release))
            (should (equal (plist-get (aura-lang-version) :version) "0.2.6"))))
      (delete-file script))))

(ert-deftest aura-lang-executable-order ()
  "Discovery order is defcustom, PATH, ~/.local/bin, ~/.aura/current/bin."
  (let ((aura-lang-executable nil))
    (should (equal (aura-lang--executable-candidates)
                   (delq nil
                         (list (executable-find "aura")
                               (let ((p (expand-file-name "~/.local/bin/aura")))
                                 (and (file-executable-p p) p))
                               (let ((p (expand-file-name "~/.aura/current/bin/aura")))
                                 (and (file-executable-p p) p))))))))

(ert-deftest aura-lang-executable-not-found ()
  "A missing binary signals a clear error and runs the hook."
  (let ((hit nil)
        (executable-find (lambda (&rest _) nil)))
    (cl-letf (((symbol-function 'file-executable-p) (lambda (&rest _) nil)))
      (let ((aura-lang-executable nil)
            (aura-lang-executable-not-found-functions
             (list (lambda () (setq hit t)))))
        (should-error (aura-lang-executable) :type 'user-error)
        (should hit)))))

(ert-deftest aura-lang-version-parsing ()
  "aura --version output is parsed for release and nightly builds."
  (should (equal (plist-get (aura-lang-parse-version "aura 0.2.6") :version) "0.2.6"))
  (should (eq (plist-get (aura-lang-parse-version "aura 0.2.6") :kind) 'release))
  (should (equal (plist-get (aura-lang-parse-version "aura nightly (cf7353373)") :commit)
                 "cf7353373"))
  (should (eq (plist-get (aura-lang-parse-version "aura nightly (cf7353373)") :kind)
              'nightly))
  (should-not (aura-lang-parse-version "")))

(ert-deftest aura-lang-lsp-registration ()
  "Both clients are declared with the aura language id and the lsp command."
  (should (equal (aura-lang-lsp-command)
                 (cons (aura-lang-executable) '("lsp"))))
  (should-not (featurep 'lsp-mode))
  (require 'eglot)
  (let ((entry (assq 'aura-mode eglot-server-programs)))
    (should entry)
    (should (equal (funcall (cdr entry) nil)
                   (aura-lang-lsp-command)))))

(ert-deftest aura-lang-repl-error-face ()
  "The REPL's - : unknown = output is shown as an error."
  (with-temp-buffer
    (inferior-aura-mode)
    (insert "- : unknown = car: not a pair\n")
    (font-lock-ensure)
    (goto-char (point-min))
    (search-forward "car")
    (should (eq (get-text-property (match-beginning 0) 'face) 'error))))

(ert-deftest aura-lang-aura-path-passthrough ()
  "aura-lang-path sets AURA_PATH only when the environment has none."
  (let ((aura-lang-path "/opt/aura/lib"))
    (if (getenv "AURA_PATH")
        (should (equal (aura-lang--child-environment) process-environment))
      (should (member "AURA_PATH=/opt/aura/lib"
                      (aura-lang--child-environment))))))

(ert-deftest aura-lang-eglot-diagnostics ()
  "Eglot starts aura lsp and receives diagnostics for a .aura buffer."
  (skip-unless (executable-find "aura"))
  (require 'eglot)
  (require 'flymake)
  (let* ((root (make-temp-file "aura-eglot" t))
         (file (expand-file-name "main.aura" root))
         (buffer nil)
         (server nil))
    (unwind-protect
        (progn
          (with-temp-file (expand-file-name "aura.toml" root)
            (insert "[package]\nname = \"demo\"\n"))
          (with-temp-file file
            (insert "(define (f x)\n  (memq x 1))\n"))
          (setq buffer (find-file-noselect file))
          (with-current-buffer buffer
            (flymake-mode 1)
            (apply #'eglot--connect (eglot--guess-contact))
            (let ((tries 50))
              (while (and (> tries 0) (not (eglot-current-server)))
                (accept-process-output nil 0.2)
                (setq tries (1- tries))))
            (setq server (eglot-current-server))
            (should server)
            (eglot--maybe-activate-editing-mode)
            (flymake-start)
            ;; #5359: a client that declares textDocument.diagnostic gets the
            ;; same set through pull, not push, so read what eglot reports to
            ;; flymake instead of grepping the event log for
            ;; textDocument/publishDiagnostics.
            (let ((tries 50))
              (while (and (> tries 0) (null (flymake-diagnostics)))
                (accept-process-output nil 0.2)
                (setq tries (1- tries))))
            (let ((diagnostics (flymake-diagnostics)))
              (should diagnostics)
              ;; #5364: the message carries the CLI wording.
              (should (seq-some
                       (lambda (diagnostic)
                         (and (string-match-p "unbound variable: memq"
                                              (flymake-diagnostic-text diagnostic))
                              (equal "memq"
                                     (buffer-substring-no-properties
                                      (flymake-diagnostic-beg diagnostic)
                                      (flymake-diagnostic-end diagnostic)))))
                       diagnostics)))
            (with-current-buffer (jsonrpc-events-buffer server)
              (should (save-excursion
                        (goto-char (point-min))
                        (search-forward "\"languageId\":\"aura\"" nil t))))))
      (when (and server (jsonrpc--process server))
        (delete-process (jsonrpc--process server)))
      (when (buffer-live-p buffer) (kill-buffer buffer))
      (delete-directory root t))))

(ert-deftest aura-lang-repl-multiline ()
  "A multi-line definition is evaluated as one form over a PTY."
  (skip-unless (executable-find "aura"))
  (let ((buffer (run-aura)))
    (unwind-protect
        (progn
          (aura-lang--send-string "(define (triple x)\n  (* x 3))")
          (aura-lang--send-string "(triple 14)")
          (let ((deadline (+ (float-time) 15)))
            (while (and (< (float-time) deadline)
                        (not (with-current-buffer buffer
                               (save-excursion
                                 (goto-char (point-max))
                                 (search-backward "42" nil t)))))
              (accept-process-output (get-buffer-process buffer) 0.2)))
          (with-current-buffer buffer
            (should (save-excursion
                      (goto-char (point-max))
                      (search-backward "- : Any = 42" nil t)))))
      (when (comint-check-proc buffer)
        (set-process-query-on-exit-flag (get-buffer-process buffer) nil)
        (kill-process (get-buffer-process buffer)))
      (let ((kill-buffer-query-functions nil))
        (kill-buffer buffer)))))

(ert-deftest aura-lang-repl-send-defun-multiline ()
  "send-defun sends a multi-line definition as one form."
  (skip-unless (executable-find "aura"))
  (let ((source (generate-new-buffer " *aura-defun*"))
        (repl nil))
    (unwind-protect
        (progn
          (setq repl (run-aura))
          (with-current-buffer source
            (insert "(define (triple x)\n  (* x 3))\n")
            (goto-char (point-min))
            (aura-lang-send-defun))
          (aura-lang--send-string "(triple 14)")
          (let ((deadline (+ (float-time) 20)))
            (while (and (< (float-time) deadline)
                        (not (with-current-buffer repl
                               (save-excursion
                                 (goto-char (point-max))
                                 (search-backward "- : Any = 42" nil t)))))
              (accept-process-output (get-buffer-process repl) 0.2)))
          (with-current-buffer repl
            (should (save-excursion
                      (goto-char (point-max))
                      (search-backward "- : Any = 42" nil t)))))
      (when (buffer-live-p source) (kill-buffer source))
      (when (and repl (comint-check-proc repl))
        (set-process-query-on-exit-flag (get-buffer-process repl) nil)
        (kill-process (get-buffer-process repl)))
      (when (and repl (buffer-live-p repl))
        (let ((kill-buffer-query-functions nil))
          (kill-buffer repl))))))

(ert-deftest aura-lang-repl-car-error ()
  "(car 1) reaches the REPL as a primitive error, not a plain value."
  (skip-unless (executable-find "aura"))
  (let ((buffer (run-aura)))
    (unwind-protect
        (progn
          (aura-lang--send-string "(car 1)")
          (let ((deadline (+ (float-time) 20)))
            (while (and (< (float-time) deadline)
                        (not (with-current-buffer buffer
                               (save-excursion
                                 (goto-char (point-max))
                                 (search-backward "car: not a pair" nil t)))))
              (accept-process-output (get-buffer-process buffer) 0.2)))
          (with-current-buffer buffer
            (should (save-excursion
                      (goto-char (point-max))
                      (search-backward "car: not a pair" nil t)))
            ;; The PTY REPL prints errors as "- : <type> = <message>",
            ;; with the prompt on the same buffer line.  The exact type
            ;; name is a server detail; the error text is the contract.
            (should (save-excursion
                      (goto-char (point-max))
                      (search-backward "car: not a pair" nil t)
                      (string-match-p
                       "- : [^=]+= car: not a pair"
                       (buffer-substring-no-properties
                        (line-beginning-position) (line-end-position)))))))
      (when (comint-check-proc buffer)
        (set-process-query-on-exit-flag (get-buffer-process buffer) nil)
        (kill-process (get-buffer-process buffer)))
      (let ((kill-buffer-query-functions nil))
        (kill-buffer buffer)))))

(ert-deftest aura-lang-repl-primitive-error-classification ()
  "The REPL's error face and a plain value stay distinguishable.
#5349: the type name is a server detail (a nightly printed list-elem for
a car error), so the keywords must key on the error value's own shape:
both (car 1) and (vector-ref ...) are errors, while (+ 1 2) and
(list 1) are values."
  (skip-unless (executable-find "aura"))
  (let* ((buffer (run-aura))
         (result-line
         (lambda (needle)
           (let ((deadline (+ (float-time) 20)))
             (while (and (< (float-time) deadline)
                         (not (with-current-buffer buffer
                                (save-excursion
                                  (goto-char (point-max))
                                  (search-backward needle nil t)))))
               (accept-process-output (get-buffer-process buffer) 0.2))
             (with-current-buffer buffer
               (save-excursion
                 (goto-char (point-max))
                 (search-backward needle nil t)
                 (let ((line (buffer-substring-no-properties
                              (line-beginning-position) (line-end-position))))
                   (should (string-match "- : " line))
                   (substring line (string-match "- : " line))))))))
        (error-line-p
         (lambda (line)
           (seq-some (lambda (keyword) (string-match-p (car keyword) line))
                     aura-lang-repl-font-lock-keywords))))
    (unwind-protect
        (progn
          (aura-lang--send-string "(car 1)")
          (should (funcall error-line-p (funcall result-line "car: not a pair")))
          (aura-lang--send-string "(vector-ref (vector 1) 5)")
          (should (funcall error-line-p
                           (funcall result-line "vector-ref: index 5 out of range")))
          (aura-lang--send-string "(+ 1 2)")
          (should-not (funcall error-line-p (funcall result-line "= 3")))
          (aura-lang--send-string "(list 1)")
          (should-not (funcall error-line-p (funcall result-line "= (1)"))))
      (when (comint-check-proc buffer)
        (set-process-query-on-exit-flag (get-buffer-process buffer) nil)
        (kill-process (get-buffer-process buffer)))
      (let ((kill-buffer-query-functions nil))
        (kill-buffer buffer)))))

(defun aura-lang-test--pos (needle)
  "Return the start of the first NEEDLE in the current buffer."
  (save-excursion
    (goto-char (point-min))
    (search-forward needle)
    (match-beginning 0)))

(defun aura-lang-test--defun-points ()
  "Return every `beginning-of-defun' stop, from the top of the buffer.
The walk starts at `point-max' and repeats with argument 1."
  (goto-char (point-max))
  (let (points)
    (while (beginning-of-defun 1)
      (push (point) points))
    points))

(ert-deftest aura-lang-defun-motion-setup ()
  "Defun motion uses real sexps, not an open parenthesis in column 0."
  (aura-lang-test--in-buffer
   "(define (a) 1)\n"
   (lambda ()
     (should-not open-paren-in-column-0-is-defun-start)
     (should-not end-of-defun-moves-to-eol)
     (should (eq beginning-of-defun-function #'aura-lang-beginning-of-defun))
     (should (eq end-of-defun-function #'aura-lang--forward-defun-end))
     (should-not aura-lang-defun-only-definitions))))

(ert-deftest aura-lang-defun-toplevel-shapes ()
  "Column 0, indented forms and several forms on one line are all stops.
Argument 1 moves to the previous start, a negative argument to the
next, and repeating either walks every stop."
  (let ((text (concat "(define (a) 1)\n"
                      "  (define (indented-top) 2)\n"
                      "(define (b) 3) (define (c) 4)\n")))
    (aura-lang-test--in-buffer
     text
     (lambda ()
       (let ((a (aura-lang-test--pos "(define (a)"))
             (indented (aura-lang-test--pos "(define (indented-top)"))
             (b (aura-lang-test--pos "(define (b)"))
             (c (aura-lang-test--pos "(define (c)")))
         (should (equal (aura-lang-test--defun-points) (list a indented b c)))
         (goto-char (1+ c))
         (should (beginning-of-defun 1))
         (should (= (point) c))
         (should (beginning-of-defun 1))
         (should (= (point) b))
         (goto-char (1+ c))
         (should (beginning-of-defun 2))
         (should (= (point) b))
         (goto-char (1+ c))
         (should-not (beginning-of-defun 10))
         (should (= (point) (point-min)))
         (goto-char a)
         (let ((origin (point)))
           (should-not (beginning-of-defun 0))
           (should (= (point) origin)))
         (goto-char a)
         (should (beginning-of-defun -1))
         (should (= (point) indented))
         (should (beginning-of-defun -1))
         (should (= (point) b))
         (should (beginning-of-defun -1))
         (should (= (point) c))
         (should-not (beginning-of-defun -1))
         (should (= (point) (point-max)))
         (goto-char a)
         (should (aura-lang-next-defun 2))
         (should (= (point) b))
         ;; Inside (b), not on its opening parenthesis.
         (goto-char (+ b 9))
         (should (beginning-of-defun 1))
         (should (= (point) b))
         (goto-char a)
         (should (beginning-of-defun -2))
         (should (= (point) b)))))))

(ert-deftest aura-lang-defun-skips-non-code-parens ()
  "A parenthesis in a string, comment or #; datum is not a stop.
`#\\(' and `#\\\"' are character literals.  `std:foo' and `[]' are
sexps, and a parenthesis nested in brackets is not a top-level stop."
  (let ((text (concat "\"\n(\n\"\n"
                      "; (line)\n"
                      "#|\n(\n|#\n"
                      "#;\n(not-a-stop)\n"
                      "#;(outer\n(inner-at-bol)\n)\n"
                      "#\\(\n"
                      "#\\\"\n"
                      "std:foo\n"
                      "[1 (2) 3]\n"
                      "(define (visible) 1)\n")))
    (aura-lang-test--in-buffer
     text
     (lambda ()
       (let ((points (aura-lang-test--defun-points))
             (string (aura-lang-test--pos "\"\n"))
             (datum (aura-lang-test--pos "#;\n"))
             (outer (aura-lang-test--pos "#;(outer"))
             (char-open (aura-lang-test--pos "#\\("))
             (char-quote (aura-lang-test--pos "#\\\""))
             (symbol (aura-lang-test--pos "std:foo"))
             (brackets (aura-lang-test--pos "[1"))
             (visible (aura-lang-test--pos "(define (visible)")))
         (should (equal points
                        (list string datum outer char-open char-quote
                              symbol brackets visible)))
         (dolist (hidden '("(not-a-stop)" "(inner-at-bol)" "(2)" "(line)"))
           (should-not (member (aura-lang-test--pos hidden) points)))
         ;; The escaped parenthesis and quote are inside the literal.
         (should (= (char-after char-open) ?#))
         (should (= (char-after char-quote) ?#))
         ;; The escaped parenthesis and quote are not stops of their own.
         (should-not (member (+ char-open 2) points))
         (should-not (member (+ char-quote 2) points))
         (should-not (member (+ string 2) points))
         (should-not (member (+ (aura-lang-test--pos "#|") 3) points)))))))

(ert-deftest aura-lang-defun-only-definitions ()
  "With the option, motion stops only on `aura-lang-define-forms'."
  (let ((text (concat "(require \"std/core\")\n"
                      "std:foo\n"
                      "[1 2]\n"
                      "42\n"
                      "(define (a) 1)\n"
                      "(display 1)\n"
                      "(define-syntax (m x) x)\n"
                      "(define-hygienic-macro (h y) y)\n"))
        (aura-lang-defun-only-definitions t))
    (aura-lang-test--in-buffer
     text
     (lambda ()
       (let ((points (aura-lang-test--defun-points)))
         (should (equal points
                        (list (aura-lang-test--pos "(define (a)")
                              (aura-lang-test--pos "(define-syntax")
                              (aura-lang-test--pos "(define-hygienic-macro"))))
         (dolist (skipped '("(require" "std:foo" "[1" "42" "(display"))
           (should-not (member (aura-lang-test--pos skipped) points)))
         (goto-char (point-max))
         (should (beginning-of-defun 1))
         (should (looking-at "(define-hygienic-macro"))
         (should-not (beginning-of-defun -1))
         (should (= (point) (point-max))))))))

(ert-deftest aura-lang-defun-end-roundtrip ()
  "end-of-defun and beginning-of-defun meet on the same sexp.
The end of a form that shares its line is that form's end, and
starting at the opening parenthesis does not skip the sexp."
  (let ((text (concat "(define (a) 1)\n"
                      "  (define (indented-top) 2)\n"
                      "(define (b) 3) (define (c) 4)\n")))
    (aura-lang-test--in-buffer
     text
     (lambda ()
       (goto-char (1+ (aura-lang-test--pos "(define (indented-top)")))
       (let ((start (save-excursion (beginning-of-defun 1) (point))))
         (end-of-defun 1)
         (should (eq (char-before) ?\)))
         (beginning-of-defun 1)
         (should (= (point) start)))
       (goto-char (aura-lang-test--pos "(define (c)"))
       (let ((start (point)))
         (end-of-defun 1)
         (should (eq (char-before) ?\)))
         (should (looking-at "\n"))
         (beginning-of-defun 1)
         (should (= (point) start)))
       (goto-char (1+ (aura-lang-test--pos "(define (b)")))
       (end-of-defun 1)
       (should (looking-at " (define (c) 4)"))
       (beginning-of-defun 1)
       (should (looking-at "(define (b)"))
       (goto-char (point-max))
       (should (end-of-defun -1))
       (should (eq (char-before) ?\)))
       (should (end-of-defun -1))
       (should (looking-back "(define (b) 3)" (line-beginning-position)))))))

(ert-deftest aura-lang-send-defun-bounds ()
  "send-defun sends one top-level sexp, not its neighbour on the line."
  (let ((sent nil))
    (cl-letf (((symbol-function 'aura-lang--send-string)
               (lambda (text) (setq sent text))))
      (aura-lang-test--in-buffer
       (concat "(define (a) 1)\n"
               "  (define (indented-top) 2)\n"
               "(define (b) 3) (define (c) 4)\n"
               "\"\n(\n\"\n")
       (lambda ()
         (goto-char (1+ (aura-lang-test--pos "indented-top")))
         (aura-lang-send-defun)
         (should (string-match-p "(define (indented-top) 2)" sent))
         (should-not (string-match-p "(define (a)" sent))
         (should-not (string-match-p "(define (b)" sent))
         (goto-char (1+ (aura-lang-test--pos "(define (c)")))
         (aura-lang-send-defun)
         (should (string-match-p "(define (c) 4)" sent))
         (should-not (string-match-p "(define (b)" sent))
         (goto-char (aura-lang-test--pos "\"\n(\n"))
         (forward-char 2)
         (aura-lang-send-defun)
         (should (string-prefix-p "\"" sent))
         (should-not (string-prefix-p "(" sent)))))))

(ert-deftest aura-lang-defun-advice-leaves-other-modes ()
  "The bol adjustment stays in place outside `aura-mode'."
  (with-temp-buffer
    (emacs-lisp-mode)
    (setq-local open-paren-in-column-0-is-defun-start nil)
    (insert "(a) (b)")
    (goto-char (point-max))
    (beginning-of-defun)
    (should (bolp))
    (should (looking-at "(a)"))))

(provide 'aura-lang-test)
;;; aura-lang-test.el ends here
