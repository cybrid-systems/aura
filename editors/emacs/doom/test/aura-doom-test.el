;;; aura-doom-test.el --- ERT tests for the Doom :lang aura module  -*- lexical-binding: t; -*-

;;; Commentary:

;; The module's config.el and doctor.el are written against Doom's module API
;; (use-package!, modulep!, set-repl-handler!, ...), so they cannot be loaded
;; in a plain Emacs.  This file stubs that API, records what the module asks
;; for, and checks the parts that decide behaviour: the formatter version gate,
;; the +lsp hook, the localleader map, and what doctor.el reports.
;;
;; It also drives the real Aura REPL through comint when an aura binary is on
;; the PATH (skipped otherwise), which is the only part that needs the binary.
;;
;; Run from editors/emacs:
;;
;;   emacs -Q --batch -L . -l doom/test/aura-doom-test.el \
;;         -f ert-run-tests-batch-and-exit

;;; Code:

(require 'ert)
(require 'cl-lib)
(require 'seq)

(defvar aura-doom-test--dir
  (file-name-directory (or load-file-name buffer-file-name))
  "Directory of this file.")

(add-to-list 'load-path (expand-file-name "../../" aura-doom-test--dir))

;;; Doom stubs

(defvar aura-doom-test--enabled '((+lsp) (:tools lsp) (:tools eval +overlay)
                                  (:editor format +onsave) (:ui popup) (:editor evil))
  "Modules the stub of `modulep!' reports as enabled.")

(defvar aura-doom-test--calls nil
  "Alist of (FUNCTION . ARGS) the module called, newest first.")

(defun aura-doom-test--record (function args)
  "Record FUNCTION with ARGS and return ARGS."
  (push (cons function args) aura-doom-test--calls)
  args)

(defun aura-doom-test--last (function)
  "Return the arguments of the newest call to FUNCTION."
  (cdr (assq function aura-doom-test--calls)))

(defun aura-doom-test--somewhere (needle tree)
  "Return non-nil when NEEDLE is TREE or nested inside it."
  (cond ((equal needle tree) t)
        ((consp tree) (or (aura-doom-test--somewhere needle (car tree))
                          (aura-doom-test--somewhere needle (cdr tree))))
        (t nil)))

(defun aura-doom-test--modulep (modules)
  "Return non-nil when MODULES names an enabled module and flags.
MODULES is a module path optionally followed by flags; a query is true when an
enabled entry has the same head and all the queried flags."
  (seq-some (lambda (entry)
              (and (equal (car entry) (car modules))
                   (seq-every-p (lambda (flag) (memq flag entry)) (cdr modules))))
            aura-doom-test--enabled))

(defmacro modulep! (&rest modules)
  "Stub of Doom's `modulep!'; its arguments are symbols, never evaluated."
  `(aura-doom-test--modulep ',modules))

(defmacro use-package! (_package &rest args)
  "Stub of Doom's `use-package!' that only evaluates :init and :config.
Both keywords take every following form up to the next keyword, as in
use-package itself."
  (let ((forms nil)
        (rest args))
    (while rest
      (let ((key (pop rest)))
        (if (memq key '(:init :config))
            (while (and rest (not (keywordp (car rest))))
              (push (pop rest) forms))
          (pop rest))))
    `(progn ,@(nreverse forms))))

(dolist (function '(set-repl-handler! set-eval-handler! set-lookup-handlers!
                    set-popup-rule! set-formatter!))
  (eval `(defun ,function (&rest args)
            (aura-doom-test--record ',function args))
        t))

;; `map!' is a macro in Doom, and the (`:localleader ...) group is part of
;; its syntax, so the stub records the forms unevaluated.
(defmacro map! (&rest args)
  "Stub of Doom's `map!' that records its forms."
  `(aura-doom-test--record 'map! ',args))

(dolist (function '(warn! error! explain! assert!))
  (eval `(defun ,function (format &rest args)
            (aura-doom-test--record ',function (list (apply #'format format args)))
            nil)
        t))

(defun aura-doom-test--lsp! ()
  "Stub of Doom's `lsp!'.")

;;; Helpers

(defun aura-doom-test--script (directory name output)
  "Write an executable DIRECTORY/NAME that prints OUTPUT."
  (let ((path (expand-file-name name directory)))
    (with-temp-file path
      (insert "#!/bin/sh\n")
      (insert (format "echo '%s'\n" output)))
    (set-file-modes path #o755)
    path))

(defun aura-doom-test--load (file)
  "Load FILE from the module directory."
  (load (expand-file-name file (expand-file-name "../lang/aura" aura-doom-test--dir))
        nil t))

(defun aura-doom-test--load-config ()
  "Load config.el fresh, with the stubs in place.
config.el loads autoload/aura.el itself.  Doom's generated loaddefs are
not part of this path: a symlinked module never gets them."
  (setq aura-doom-test--calls nil)
  (aura-doom-test--load "config.el"))

(defun aura-doom-test--load-doctor ()
  "Load doctor.el fresh, with the stubs in place.
doctor.el loads autoload/aura.el itself, as `doom doctor' does not load
config.el and may not have the module's generated autoloads."
  (setq aura-doom-test--calls nil)
  (aura-doom-test--load "doctor.el"))

(defun aura-doom-test--unbind-gate ()
  "Remove the formatter gate so the next load must define it again."
  (when (fboundp '+aura-formatter-state)
    (fmakunbound '+aura-formatter-state))
  (dolist (symbol '(+aura-min-format-version +aura-fmt-fix-commit
                    +aura-trust-nightly-formatter))
    (when (boundp symbol)
      (makunbound symbol))))

(ert-deftest aura-doom-gate-defined-through-symlink ()
  "A symlinked module directory still defines the formatter gate.
Doom's autoload scan drops this layout.  config.el and doctor.el must
define `+aura-formatter-state' without a generated loaddefs file."
  (let* ((root (make-temp-file "aura-doom-link" t))
         (link (expand-file-name "aura" root))
         (real (file-truename
                (expand-file-name "../lang/aura" aura-doom-test--dir))))
    (unwind-protect
        (progn
          (make-symbolic-link real link)
          (should (file-symlink-p link))
          (should-not (file-in-directory-p link root))
          (aura-doom-test--unbind-gate)
          (load (expand-file-name "config.el" link) nil t)
          (should (fboundp '+aura-formatter-state))
          (should (boundp '+aura-min-format-version))
          (should (memq (plist-get (+aura-formatter-state) :state)
                        '(ok too-old nightly unparsed missing)))
          (aura-doom-test--unbind-gate)
          (setq aura-doom-test--calls nil)
          (load (expand-file-name "doctor.el" link) nil t)
          (should (fboundp '+aura-formatter-state))
          (should (boundp '+aura-min-format-version)))
      (delete-directory root t))))

(defun aura-doom-test--warnings ()
  "Return the messages warn! and error! recorded."
  (seq-filter (lambda (call) (memq (car call) '(warn! error!)))
              (reverse aura-doom-test--calls)))

;;; Version gate

(require 'aura-lang)

(defmacro aura-doom-test--with-binary (output &rest body)
  "Run BODY with \`aura-lang-executable' pointing at a fake binary printing OUTPUT.
OUTPUT nil means no executable at all."
  (declare (indent 1))
  `(let* ((directory (make-temp-file "aura-doom-" t))
          (binary (when ,output (aura-doom-test--script directory "aura" ,output))))
     (unwind-protect
         (let ((aura-lang-executable binary))
           (if binary
               (progn ,@body)
             ;; No binary: the lookup has to fail the way it does on a machine
             ;; without Aura (user-error from aura-lang-executable), and the
             ;; version parser is shadowed too because native compilation may
             ;; inline the calls it makes internally.
             (cl-letf (((symbol-function 'aura-lang-executable)
                        (lambda () (user-error "Aura executable not found")))
                       ((symbol-function 'aura-lang-version)
                        (lambda (&rest _) (user-error "Aura executable not found"))))
               (setq aura-lang-executable nil)
               ,@body)))
       (delete-directory directory t))))

(ert-deftest aura-doom-version-gate ()
  "The gate registers only binaries whose --fmt keeps comments."
  (aura-doom-test--with-binary "aura 0.2.6"
    (aura-doom-test--load-config)
    (should (eq 'too-old (plist-get (+aura-formatter-state) :state))))
  (aura-doom-test--with-binary "aura 0.2.7"
    (aura-doom-test--load-config)
    (should (eq 'ok (plist-get (+aura-formatter-state) :state))))
  (aura-doom-test--with-binary "aura 0.2.10"
    (aura-doom-test--load-config)
    (should (eq 'ok (plist-get (+aura-formatter-state) :state))))
  (aura-doom-test--with-binary "aura nightly (cf7353373)"
    (aura-doom-test--load-config)
    (should (eq 'nightly (plist-get (+aura-formatter-state) :state))))
  (aura-doom-test--with-binary "who knows"
    (aura-doom-test--load-config)
    (should (eq 'unparsed (plist-get (+aura-formatter-state) :state))))
  (aura-doom-test--with-binary nil
    (aura-doom-test--load-config)
    (should (eq 'missing (plist-get (+aura-formatter-state) :state)))))

(ert-deftest aura-doom-dev-build-gate ()
  "A source-tree build (\`aura dev (<sha>)') is not trusted by default."
  (aura-doom-test--with-binary "aura dev (cf8168575f7c8a2b660ba3ff6483d00eb9497447)"
    (aura-doom-test--load-config)
    (should (eq 'unparsed (plist-get (+aura-formatter-state) :state)))
    (should-not (aura-doom-test--last 'set-formatter!))
    (let ((+aura-trust-nightly-formatter t))
      (aura-doom-test--load-config)
      (should (eq 'ok (plist-get (+aura-formatter-state) :state))))))

(ert-deftest aura-doom-nightly-trust ()
  "The nightly can be trusted explicitly."
  (aura-doom-test--with-binary "aura nightly (cf7353373)"
    (aura-doom-test--load-config)
    (let ((+aura-trust-nightly-formatter t))
      (should (eq 'ok (plist-get (+aura-formatter-state) :state))))))

(ert-deftest aura-doom-formatter-registration ()
  "config.el registers +aura-fmt for 0.2.7 and for nothing older."
  (aura-doom-test--with-binary "aura 0.2.7"
    (aura-doom-test--load-config)
    (should (aura-doom-test--somewhere '+aura-fmt (aura-doom-test--last 'set-formatter!))))
  (aura-doom-test--with-binary "aura 0.2.6"
    (aura-doom-test--load-config)
    (should-not (aura-doom-test--last 'set-formatter!)))
  (aura-doom-test--with-binary "aura nightly (cf7353373)"
    (aura-doom-test--load-config)
    (should-not (aura-doom-test--last 'set-formatter!))
    (let ((+aura-trust-nightly-formatter t))
      (aura-doom-test--load-config)
      (should (aura-doom-test--last 'set-formatter!))))
  (aura-doom-test--with-binary nil
    (aura-doom-test--load-config)
    (should-not (aura-doom-test--last 'set-formatter!))))

;;; Wiring

(ert-deftest aura-doom-lsp-hook ()
  "The +lsp flag adds \`lsp!' to the mode's local-vars hook, and only with the flag."
  (aura-doom-test--with-binary "aura 0.2.7"
    (let ((aura-doom-test--enabled '((:tools lsp))))
      (setq aura-mode-local-vars-hook nil)
      (aura-doom-test--load-config)
      (should-not (memq 'lsp! aura-mode-local-vars-hook)))
    (let ((aura-doom-test--enabled '((+lsp) (:tools lsp))))
      (setq aura-mode-local-vars-hook nil)
      (aura-doom-test--load-config)
      (should (memq 'lsp! aura-mode-local-vars-hook)))))

(ert-deftest aura-doom-handlers ()
  "REPL, eval and lookup are registered for aura-mode."
  (aura-doom-test--with-binary "aura 0.2.7"
    (aura-doom-test--load-config)
    (should (eq 'aura-mode (car (aura-doom-test--last 'set-repl-handler!))))
    (should (eq 'run-aura (cadr (aura-doom-test--last 'set-repl-handler!))))
    (should (eq 'aura-mode (car (aura-doom-test--last 'set-eval-handler!))))
    (should (eq '+aura-eval-region (cadr (aura-doom-test--last 'set-eval-handler!))))
    (should (aura-doom-test--somewhere '+aura-lookup-definition
                                       (aura-doom-test--last 'set-lookup-handlers!)))
    (should (aura-doom-test--somewhere '+aura-lookup-documentation
                                       (aura-doom-test--last 'set-lookup-handlers!)))))

(ert-deftest aura-doom-localleader ()
  "The localleader holds goto, send, world, REPL and doc keys."
  (aura-doom-test--with-binary "aura 0.2.7"
    (aura-doom-test--load-config)
    (let ((keys (aura-doom-test--last 'map!)))
      (dolist (expected '(("s" . "send") ("g" . "goto") ("w" . "world")
                          "s" "d" "r" "b" "w" "k" "'"
                          +aura-send-defun +aura-send-region +aura-send-buffer
                          run-aura +lookup/definition
                          aura-lang-repl-worldline aura-lang-repl-snapshot
                          aura-lang-repl-restore aura-lang-repl-ws
                          aura-lang-reload-file +aura-lookup-documentation))
        (should (aura-doom-test--somewhere expected keys))))))

;;; Doctor

(ert-deftest aura-doom-doctor-missing-binary ()
  "doctor says doom env when the binary cannot be found."
  (aura-doom-test--with-binary nil
    (aura-doom-test--load-doctor)
    (let ((text (mapconcat (lambda (call) (cadr call)) (aura-doom-test--warnings) " ")))
      (should (string-match-p "doom env" text)))))

(ert-deftest aura-doom-doctor-old-version ()
  "doctor explains the missing formatter on 0.2.6."
  (aura-doom-test--with-binary "aura 0.2.6"
    (aura-doom-test--load-doctor)
    (let ((text (mapconcat (lambda (call) (cadr call)) (aura-doom-test--warnings) " ")))
      (should (string-match-p "0\\.2\\.7" text))
      (should (string-match-p "c04a44c3e" text))
      (should (string-match-p "+onsave" text)))))

(ert-deftest aura-doom-doctor-nightly ()
  "doctor says why a nightly gets no formatter."
  (aura-doom-test--with-binary "aura nightly (cf7353373)"
    (aura-doom-test--load-doctor)
    (let ((text (mapconcat (lambda (call) (cadr call)) (aura-doom-test--warnings) " ")))
      (should (string-match-p "nightly" text))
      (should (string-match-p "\\+aura-trust-nightly-formatter" text)))))

(ert-deftest aura-doom-doctor-lsp-without-module ()
  "doctor reports +lsp without :tools lsp."
  (aura-doom-test--with-binary "aura 0.2.7"
    (let ((aura-doom-test--enabled '((+lsp) (:editor format +onsave))))
      (aura-doom-test--load-doctor)
      (let ((text (mapconcat (lambda (call) (cadr call))
                             (aura-doom-test--warnings) " ")))
        (should (string-match-p ":tools lsp" text))))))

(ert-deftest aura-doom-doctor-quiet-when-fine ()
  "doctor warns about the formatter only when something is wrong."
  (aura-doom-test--with-binary "aura 0.2.7"
    (aura-doom-test--load-doctor)
    (should-not (aura-doom-test--warnings))))

;;; The real REPL (needs an aura binary)

(ert-deftest aura-doom-repl-roundtrip ()
  "A send returns the REPL's answer, and a primitive error is recognised.
This is the only test that needs a real aura binary; it starts one REPL
process on a PTY, the way the module does."
  (skip-unless (executable-find "aura"))
  (let ((aura-lang-executable (executable-find "aura")))
    (aura-doom-test--load-config)
    (unwind-protect
        (progn
          ;; The PTY REPL answers with the value and its type: "- : Int = 3".
          (let ((answer (+aura--send "(+ 1 2)")))
            (should answer)
            (should (string-match-p "3\\'" answer))
            (should-not (+aura--error-output-p answer)))
          (let ((answer (+aura--send "(car 1)")))
            (should answer)
            (should (string-match-p "car: not a pair" answer))
            (should (+aura--error-output-p answer))))
      (when-let* ((buffer (get-buffer aura-lang-repl-buffer)))
        (when-let* ((process (get-buffer-process buffer)))
          (set-process-query-on-exit-flag process nil)
          (delete-process process))
        (kill-buffer buffer)))))

(provide 'aura-doom-test)
;;; aura-doom-test.el ends here
