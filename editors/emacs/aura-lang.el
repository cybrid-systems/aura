;;; aura-lang.el --- Aura language support (major mode, REPL, LSP) -*- lexical-binding: t; -*-

;; Copyright (C) 2026 Cybrid Systems
;;
;; Author: Cybrid Systems
;; URL: https://github.com/cybrid-systems/aura
;; Version: 0.2.7
;; Package-Requires: ((emacs "29.1"))
;; Keywords: languages, lisp
;; SPDX-License-Identifier: Apache-2.0

;;; Commentary:

;; Emacs support for the Aura language (https://github.com/cybrid-systems/aura).
;;
;;   - aura-mode for .aura files, derived from lisp-data-mode.
;;   - inferior-aura-mode / run-aura: a comint REPL over a PTY.
;;   - Optional registration with lsp-mode and eglot ("aura lsp").
;;
;; The package does not load lsp-mode or eglot; it only registers a client
;; in each of them once that package is loaded.
;;
;; Reader notes (probed with aura 0.2.6/nightly):
;;   * ';' is a line comment, '#| ... |#' a block comment.
;;   * '#;' is NOT a comment; the reader reports a parse error.
;;   * '#\a' is a character literal (97), ':kw' a self-evaluating keyword.
;;   * '[' and ']' parse, but they are not list literals: (display [1 2 3])
;;     prints 3 and (car [1 2 3]) is a parse error.  Their semantics are
;;     still open (issue #5264 / the A6 formatter work), so this mode treats
;;     '[' and ']' as ordinary matching brackets and gives them no list
;;     syntax.  Revisit when the reader semantics are settled.

;;; Code:

(require 'cl-lib)
(require 'comint)
(require 'json)
(require 'lisp-mode)
(require 'project)
(require 'seq)
(require 'subr-x)

(defgroup aura-lang nil
  "Major mode, REPL and LSP glue for the Aura language."
  :group 'languages
  :prefix "aura-lang-"
  :link '(url-link "https://github.com/cybrid-systems/aura"))

(defcustom aura-lang-executable nil
  "Path to the Aura executable.
When nil, Aura is searched for in PATH, then ~/.local/bin/aura, then
~/.aura/current/bin/aura."
  :type '(choice (const :tag "Search automatically" nil) file)
  :group 'aura-lang)

(defcustom aura-lang-path nil
  "Value for the AURA_PATH environment variable in Aura child processes.
When nil, an existing AURA_PATH is passed through unchanged.  Aura's
launcher already appends its own library directory, so this is only
needed for extra library roots."
  :type '(choice (const :tag "Pass through the environment" nil) string)
  :group 'aura-lang)

(defcustom aura-lang-lsp-args '("lsp")
  "Arguments passed to the Aura executable to start the language server."
  :type '(repeat string)
  :group 'aura-lang)

(defcustom aura-lang-repl-args '("repl")
  "Arguments passed to the Aura executable to start the REPL."
  :type '(repeat string)
  :group 'aura-lang)

(defcustom aura-lang-repl-buffer "*aura*"
  "Buffer name used by `run-aura'."
  :type 'string
  :group 'aura-lang)

(defcustom aura-lang-executable-not-found-functions nil
  "Hook run before Aura's executable lookup signals a missing binary.
Each function is called with no arguments.  Doom's doctor (B2) can use
this to add checks such as whether `doom env' was refreshed."
  :type 'hook
  :group 'aura-lang)

(defcustom aura-lang-grammar-file nil
  "Path to the TextMate grammar used for font-lock keywords.
When nil, the grammar is looked up next to this package in
editors/grammar/aura.tmLanguage.json (the post-#5273 location) and then
in the older editors/vscode/syntaxes/ location."
  :type '(choice (const :tag "Locate automatically" nil) file)
  :group 'aura-lang)

;;; Grammar-derived keyword lists

(defconst aura-lang--grammar-relative-paths
  '("../grammar/aura.tmLanguage.json"
    "../vscode/syntaxes/aura.tmLanguage.json")
  "Grammar locations relative to this file, in lookup order.
#5273 moves the grammar to editors/grammar/aura.tmLanguage.json; the
VS Code location stays as a fallback for older checkouts.")

(defconst aura-lang--fallback-keywords
  '("lambda" "let" "let*" "letrec" "if" "cond" "case" "when" "unless"
    "begin" "do" "set!" "quote" "quasiquote" "and" "or" "else" "require"
    "match" "try" "guard")
  "Keywords used only when the TextMate grammar cannot be found.
The grammar file is the source of truth; `aura-lang-check-grammar'
reports drift between this list and the grammar.")

(defconst aura-lang--fallback-define-forms
  '("define" "define-syntax" "define-record-type")
  "Definition forms as written in the TextMate grammar.")

(defconst aura-lang--extra-define-forms
  '("define-hygienic-macro" "define-type" "define-struct" "defmacro")
  "Definition forms the grammar does not list yet.
They are still highlighted and indented.  #5273 should add them to the
grammar; `aura-lang-check-grammar' reports the difference.")

(defconst aura-lang--base-directory
  (file-name-directory
   (or load-file-name buffer-file-name (locate-library "aura-lang")
       default-directory))
  "Directory of this package, captured at load time.")
(defun aura-lang--library-directory ()
  "Return the directory holding this file."
  aura-lang--base-directory)

(defun aura-lang--grammar-file ()
  "Return the grammar file this package should read, or nil."
  (or (and aura-lang-grammar-file (file-readable-p aura-lang-grammar-file)
           (expand-file-name aura-lang-grammar-file))
      (let ((base (aura-lang--library-directory)))
        (seq-find #'file-readable-p
                  (mapcar (lambda (rel) (expand-file-name rel base))
                          aura-lang--grammar-relative-paths)))))

(defun aura-lang--grammar-repository (file)
  "Read FILE and return its TextMate repository alist."
  (with-temp-buffer
    (insert-file-contents file)
    (let ((data (json-parse-buffer :object-type 'alist :null-object nil
                                   :false-object nil)))
      (alist-get 'repository data))))

(defun aura-lang--match-string (repository group)
  "Return the match source of GROUP in REPOSITORY."
  (let ((entry (alist-get group repository)))
    (alist-get 'match entry)))

(defun aura-lang--alternation (regexp)
  "Return the alternatives of the first (a|b) group in REGEXP.
The lookbehind at the start is skipped; the result keeps Emacs regexp
fragments such as let with an optional star intact."
  (when (stringp regexp)
    (let ((start 0) found)
      (while (and (not found)
                  (string-match "(\\([^()]*\\))" regexp start))
        (let ((body (match-string 1 regexp)))
          (if (string-match-p "|" body)
              (setq found body)
            (setq start (match-end 0)))))
      (when found (split-string found "|" t)))))

(defun aura-lang--expand-optional (frag)
  "Return the literal names denoted by the regexp fragment FRAG.
Handles the grammar's optional-suffix style (a trailing ?) so the name
can be used for indentation and imenu."
  (let ((text (replace-regexp-in-string "\\\\" "" frag)))
    (if (and (> (length text) 1) (string-suffix-p "?" text))
        (let ((base (substring text 0 (- (length text) 2)))
              (opt (substring text (- (length text) 2) (1- (length text)))))
          (list base (concat base opt)))
      (list text))))

(defun aura-lang--names-from (regexp)
  "Return literal names from the alternation in REGEXP."
  (let ((alts (aura-lang--alternation regexp)))
    (when alts
      (delete-dups (apply #'append (mapcar #'aura-lang--expand-optional alts))))))

(defvar aura-lang--grammar-keywords nil
  "Keyword names read from the grammar, or nil when none was found.")
(defvar aura-lang--grammar-define-forms nil
  "Definition forms read from the grammar, or nil when none was found.")

(let ((aura-lang--file (aura-lang--grammar-file)))
  (when aura-lang--file
    (condition-case err
        (let ((repo (aura-lang--grammar-repository aura-lang--file)))
          (setq aura-lang--grammar-keywords
                (aura-lang--names-from (aura-lang--match-string repo 'keyword))
                aura-lang--grammar-define-forms
                (aura-lang--names-from (aura-lang--match-string repo 'define))))
      (error
       (display-warning 'aura-lang
                        (format "Could not read %s: %s"
                                aura-lang--file (error-message-string err))
                        :warning)))))

(defconst aura-lang-keywords
  (or aura-lang--grammar-keywords aura-lang--fallback-keywords)
  "Keywords highlighted by `aura-mode'.")

(defconst aura-lang-define-forms
  (append (or aura-lang--grammar-define-forms aura-lang--fallback-define-forms)
          aura-lang--extra-define-forms)
  "Definition forms highlighted by `aura-mode'.")

(defun aura-lang-check-grammar ()
  "Compare the mode's keyword lists with the TextMate grammar.
Return an alist of :file, :grammar-keywords, :grammar-defines,
:mode-keywords, :mode-defines, :missing-in-mode and
:missing-in-grammar.  Signal an error when no grammar is found."
  (interactive)
  (let ((file (aura-lang--grammar-file)))
    (unless file
      (user-error "Aura grammar not found; set aura-lang-grammar-file"))
    (let* ((repo (aura-lang--grammar-repository file))
           (gk (aura-lang--names-from (aura-lang--match-string repo 'keyword)))
           (gd (aura-lang--names-from (aura-lang--match-string repo 'define)))
           (report (list (cons :file file)
                         (cons :grammar-keywords gk)
                         (cons :grammar-defines gd)
                         (cons :mode-keywords aura-lang-keywords)
                         (cons :mode-defines aura-lang-define-forms)
                         (cons :missing-in-mode
                               (seq-difference gk aura-lang-keywords))
                         (cons :missing-defines-in-mode
                               (seq-difference gd aura-lang-define-forms))
                         (cons :extra-in-mode
                               (seq-difference
                                (append aura-lang-keywords aura-lang-define-forms)
                                (append gk gd))))))
      (when (called-interactively-p 'interactive)
        (message "%s" report))
      report)))

;;; Syntax table

(defvar aura-lang-mode-syntax-table
  (let ((table (make-syntax-table lisp-data-mode-syntax-table)))
    ;; Line comments: ";" to end of line.
    (modify-syntax-entry ?\; "< b" table)
    (modify-syntax-entry ?\n "> b" table)
    ;; Block comments "#| ... |#".  The "b" comment style is shared with
    ;; the line comments so that both work in one table.
    (modify-syntax-entry ?# ". 124b" table)
    (modify-syntax-entry ?| ". 23" table)
    ;; Strings, characters, escapes.
    (modify-syntax-entry ?\" "\"" table)
    (modify-syntax-entry ?\\ "\\" table)
    ;; [ ] are matching brackets but not list syntax (see Commentary).
    (modify-syntax-entry ?\[ "(]" table)
    (modify-syntax-entry ?\] ")[" table)
    table)
  "Syntax table for `aura-mode'.")

(defun aura-lang-syntax-propertize (start end)
  "Make #; a non-comment between START and END.
The reader rejects #; (unlike Scheme), so the semicolon must not start
a line comment in the mode either."
  (save-excursion
    (goto-char start)
    (while (re-search-forward "#;" end t)
      (let ((state (save-excursion (syntax-ppss (match-beginning 0)))))
        (unless (or (nth 3 state) (nth 4 state))
          (put-text-property (1- (match-end 0)) (match-end 0)
                             'syntax-table (string-to-syntax ".")))))))

;;; Font-lock

(defun aura-lang--keyword-regexp (names)
  "Return a matcher for the symbol names NAMES after an opening paren."
  (concat "(\\_<\\(" (mapconcat #'regexp-quote names "\\|") "\\)\\_>"))

(defconst aura-lang-font-lock-keywords
  (list (list (aura-lang--keyword-regexp aura-lang-keywords)
              1 'font-lock-keyword-face)
        (list (concat "(\\_<\\("
                      (mapconcat #'regexp-quote aura-lang-define-forms "\\|")
                      "\\)\\_>[ \t]+\\(?:([ \t]*\\)?\\([^][() \t\n\"';,]+\\)")
              (list 1 'font-lock-keyword-face)
              (list 2 'font-lock-function-name-face))
        ;; #t / #f / #true / #false
        (list "\\(?:^\\|[^[:alnum:]_#-]\\)\\(#\\(?:t\\|f\\|true\\|false\\)\\_>\\)"
              1 'font-lock-constant-face)
        ;; #\a / #\space / #\newline / #\tab
        (list "#\\\\\\(?:space\\|newline\\|tab\\|.\\)"
              0 'font-lock-constant-face)
        ;; :kw keywords
        (list "\\(?:^\\|[^[:alnum:]_-]\\)\\(:[[:alpha:]][[:alnum:]_-]*\\)"
              1 'font-lock-constant-face)
        ;; numbers
        (list "\\(?:^\\|[^[:alnum:]_-]\\)\\(-?[0-9]+\\(?:\\.[0-9]+\\)?\\(?:[eE][+-]?[0-9]+\\)?\\)\\(?:[^[:alnum:]_-]\\|$\\)"
              1 'font-lock-constant-face)
        ;; mutate:foo and friends
        (list "\\(?:^\\|[^[:alnum:]-]\\)\\(\\(?:mutate\\|ast\\|evo\\|workspace\\|mutation-log\\):[^][() \t\n\"';,]+\\)"
              1 'font-lock-builtin-face))
  "Font-lock keywords for `aura-mode'.
Keywords and definition forms come from the TextMate grammar; see
`aura-lang-check-grammar'.")

;;; Indentation

(defconst aura-lang--indent-specs
  '((define . 1)
    (define-syntax . 1)
    (define-record-type . 1)
    (define-hygienic-macro . 1)
    (define-type . 1)
    (define-struct . 1)
    (defmacro . 1)
    (lambda . 1)
    (let . 1)
    (let* . 1)
    (letrec . 1)
    (when . 1)
    (unless . 1)
    (if . 1)
    (cond . 0)
    (case . 1)
    (match . 1)
    (try . 0)
    (guard . 1)
    (begin . 0)
    (do . 0)
    (and . 1)
    (or . 1)
    (require . 0))
  "Indentation rules for Aura forms.
Values follow the symbol `lisp-indent-function': the number of distinguished
arguments before the body, so bodies indent by two columns.  The A6
formatter is not in this tree yet; aura --fmt normalises let/when/cond
bodies to +2 columns, which is what these values give.")

(defun aura-lang--install-indent-specs ()
  "Give Aura forms their symbol `lisp-indent-function' properties."
  (dolist (spec aura-lang--indent-specs)
    (put (car spec) 'lisp-indent-function (cdr spec))))

(aura-lang--install-indent-specs)

;;; Imenu

(defconst aura-lang-imenu-generic-expression
  (list
   (list nil
         (concat "^(\\_<\\("
                 (mapconcat #'regexp-quote aura-lang-define-forms "\\|")
                 "\\)\\_>[ \t]+\\(?:([ \t]*\\)?\\([^][() \t\n\"';,]+\\)")
         2))
  "Imenu fallback for `aura-mode', used when no LSP is running.")

;;; Major mode

;;;###autoload
(define-derived-mode aura-mode lisp-data-mode "Aura"
  "Major mode for Aura source files.

Comments use ; and #| ... |#; #; is not a comment.  Font-lock keywords
come from the TextMate grammar in this repository.  Indentation uses
the symbol `lisp-indent-function' (see `aura-lang--indent-specs')."
  :group 'aura-lang
  :syntax-table aura-lang-mode-syntax-table
  (setq-local font-lock-defaults
              (list aura-lang-font-lock-keywords nil nil nil nil))
  (setq-local syntax-propertize-function #'aura-lang-syntax-propertize)
  (setq-local imenu-generic-expression aura-lang-imenu-generic-expression)
  (setq-local imenu-case-fold-search nil)
  (setq-local comment-start ";")
  (setq-local comment-end "")
  (setq-local comment-start-skip ";[ \t]*")
  (setq-local comment-use-syntax t))

;;;###autoload
(add-to-list 'auto-mode-alist '("\\.aura\\'" . aura-mode))

;;; Project integration

;;;###autoload
(defun aura-lang-project-root (dir)
  "Return the Aura project root for DIR, or nil.
An aura.toml file marks the root; it is registered with project.el as a
transient project so `project-current' works outside VC."
  (when-let* ((root (locate-dominating-file dir "aura.toml")))
    (cons 'transient (file-name-as-directory (expand-file-name root)))))

;; `project-find-functions' lives in project.el, which Emacs does not preload.
;; An autoload cookie copies this form into the package autoloads file, and
;; that file is loaded at startup (`emacs -Q', package-quickstart, straight,
;; `doom install') before project.el exists.  Touching the variable there is
;; a void-variable error.  Defer the registration until project.el is loaded;
;; `add-to-list' does not add a duplicate when both this form and a later
;; load of this file run.
;;;###autoload
(with-eval-after-load 'project
  (add-to-list 'project-find-functions #'aura-lang-project-root))

(add-to-list 'project-vc-extra-root-markers "aura.toml")

(with-eval-after-load 'projectile
  (defvar projectile-project-root-files)
  (add-to-list 'projectile-project-root-files "aura.toml" t))

;;; Executable discovery

(defun aura-lang--executable-candidates ()
  "Return the executable candidates in discovery order."
  (delq nil
        (list aura-lang-executable
              (executable-find "aura")
              (let ((p (expand-file-name "~/.local/bin/aura")))
                (and (file-executable-p p) p))
              (let ((p (expand-file-name "~/.aura/current/bin/aura")))
                (and (file-executable-p p) p)))))

(defun aura-lang-executable ()
  "Return the Aura executable to use, or signal a clear error.
The order is `aura-lang-executable', PATH, ~/.local/bin/aura, then
~/.aura/current/bin/aura."
  (or (cl-find-if #'file-executable-p (aura-lang--executable-candidates))
      (progn
        (run-hooks 'aura-lang-executable-not-found-functions)
        (user-error
         (concat "Aura executable not found.  Set aura-lang-executable "
                 "or install Aura (install.sh links ~/.local/bin/aura).  "
                 "With Doom Emacs, run doom env after changing PATH")))))

(defun aura-lang--child-environment ()
  "Return `process-environment' with AURA_PATH set from `aura-lang-path'."
  (if (or (null aura-lang-path) (getenv "AURA_PATH"))
      process-environment
    (cons (concat "AURA_PATH=" aura-lang-path) process-environment)))

(defun aura-lang-lsp-command ()
  "Return the command list that starts aura lsp."
  (cons (aura-lang-executable) aura-lang-lsp-args))

(defun aura-lang-repl-command ()
  "Return the command list that starts the Aura REPL."
  (cons (aura-lang-executable) aura-lang-repl-args))

;;; Version

(defun aura-lang-parse-version (output)
  "Parse the OUTPUT of aura --version into a plist.
Release builds such as \"aura 0.2.6\" yield a plist with :kind
release and :version \"0.2.6\"; nightlies such as
\"aura nightly (cf7353373)\" yield :kind nightly and :commit
\"cf7353373\"."
  (let ((text (string-trim (or output ""))))
    (cond
     ((string-match "nightly[ \t]*(\\([0-9a-fA-F]+\\))" text)
      (list :kind 'nightly :commit (match-string 1 text) :raw text))
     ((string-match "\\([0-9]+\\.[0-9]+\\.[0-9]+\\)" text)
      (list :kind 'release :version (match-string 1 text) :raw text))
     ((string-empty-p text) nil)
     (t (list :kind 'unknown :raw text)))))

(defun aura-lang-version (&optional executable)
  "Return `aura-lang-parse-version' for EXECUTABLE's --version output."
  (with-temp-buffer
    (let ((status (call-process (or executable (aura-lang-executable))
                                nil t nil "--version")))
      (unless (eq 0 status)
        (error "Aura --version failed with status %s" status))
      (aura-lang-parse-version (buffer-string)))))

;;; LSP registration

(declare-function lsp-register-client "lsp-mode" (client))
(declare-function make-lsp-client "lsp-mode" (&rest args))
(declare-function lsp-stdio-connection "lsp-mode" (command &optional test-command))
(declare-function lsp--set-configuration "lsp-mode" (settings))
(declare-function lsp-configuration-section "lsp-mode" (section))
(declare-function with-lsp-workspace "lsp-mode" (workspace &rest body))
(defvar lsp-language-id-configuration)

(with-eval-after-load 'lsp-mode
  (add-to-list 'lsp-language-id-configuration '(aura-mode . "aura"))
  (lsp-register-client
   (make-lsp-client
    :new-connection (lsp-stdio-connection #'aura-lang-lsp-command)
    :major-modes '(aura-mode)
    :language-id "aura"
    :server-id 'aura-lsp
    ;; #5358: lsp-mode sends workspace/didChangeConfiguration only when a
    ;; client asks it to, so the aura.* values registered through
    ;; `lsp-register-custom-settings' never reached the server.  Sending the
    ;; section once the workspace is initialized makes those values (and a
    ;; later change to them) take effect.
    :initialized-fn
    (lambda (workspace)
      (with-lsp-workspace workspace
        (lsp--set-configuration (lsp-configuration-section "aura")))))))

(defvar eglot-server-programs)

(defun aura-lang-eglot-contact (_interactive)
  "Return the eglot server program for Aura.
INTERACTIVE is ignored; eglot passes it when prompting."
  (aura-lang-lsp-command))

(with-eval-after-load 'eglot
  (add-to-list 'eglot-server-programs
               (cons 'aura-mode #'aura-lang-eglot-contact)))

;;; REPL

(defvar aura-lang-repl-font-lock-keywords
  '(("^- : unknown = .*$" 0 'error t)
    ("^error: .*$" 0 'error t)
    ("^:restore .*no such snapshot.*$" 0 'error t))
  "Output patterns the REPL should show as errors.
The REPL currently reports primitive errors as a value of the form
\"- : unknown = ...\"; #5234/#5241 will make the result structured
and this can go away.")

(defun aura-lang--repl-buffer ()
  "Return a live REPL buffer, starting one if needed."
  (let ((buffer (get-buffer aura-lang-repl-buffer)))
    (unless (and buffer (comint-check-proc buffer))
      (setq buffer (run-aura)))
    buffer))

(defun aura-lang--send-string (text)
  "Send TEXT to the Aura REPL as one input."
  (let ((buffer (aura-lang--repl-buffer)))
    (with-current-buffer buffer
      (goto-char (point-max))
      (insert text)
      (unless (bolp) (insert "\n"))
      (comint-send-input nil t))))

;;;###autoload
(defun run-aura (&optional command-line)
  "Start an inferior Aura REPL in `aura-lang-repl-buffer'.
COMMAND-LINE, when non-nil, replaces `aura-lang-repl-args'.  The
process always uses a PTY: in pipe mode the REPL evaluates multi-line
forms line by line."
  (interactive)
  (let* ((buffer (get-buffer-create aura-lang-repl-buffer))
         (program (aura-lang-executable))
         (args (or command-line aura-lang-repl-args)))
    (with-current-buffer buffer
      (unless (comint-check-proc buffer)
        (let ((process-connection-type t)
              (process-environment (aura-lang--child-environment)))
          (apply #'make-comint-in-buffer "aura" buffer program nil args))
        (inferior-aura-mode)))
    (pop-to-buffer buffer)
    buffer))

(define-derived-mode inferior-aura-mode comint-mode "iAura"
  "Major mode for an inferior Aura REPL.

Output that the REPL reports as a value but is really a primitive
error, of the form \"- : unknown = ...\", is shown with the error face."
  :group 'aura-lang
  (setq-local comint-prompt-regexp "^> \\|^\\.\\. ")
  (setq-local comint-use-prompt-regexp t)
  (setq-local comint-prompt-read-only nil)
  (setq-local comint-input-sender 'comint-simple-send)
  (setq-local font-lock-defaults (list aura-lang-repl-font-lock-keywords))
  (setq-local comment-start ";")
  (setq-local comment-end "")
  (add-hook 'comint-output-filter-functions
            #'comint-postoutput-scroll-to-bottom nil t))

(defun aura-lang-send-region (start end)
  "Send the region between START and END to the Aura REPL."
  (interactive "r")
  (aura-lang--send-string (buffer-substring-no-properties start end)))

(defun aura-lang-send-defun ()
  "Send the definition at point to the Aura REPL."
  (interactive)
  (save-excursion
    (end-of-defun)
    (let ((end (point)))
      (beginning-of-defun)
      (aura-lang-send-region (point) end))))

(defun aura-lang-send-buffer ()
  "Send the whole buffer to the Aura REPL."
  (interactive)
  (aura-lang-send-region (point-min) (point-max)))

(defun aura-lang-reload-file (&optional file)
  "Ask the REPL to re-read FILE, defaulting to the visited file."
  (interactive)
  (aura-lang--send-string
   (concat ":reload " (or file buffer-file-name
                          (user-error "Buffer is not visiting a file")))))

(defun aura-lang-repl--command (command &optional argument)
  "Send COMMAND to the REPL, with ARGUMENT appended when non-nil."
  (aura-lang--send-string (if argument (concat command " " argument) command)))

(defun aura-lang-repl-ws ()
  "Show the REPL workspace."
  (interactive)
  (aura-lang-repl--command ":ws"))

(defun aura-lang-repl-worldline ()
  "Show the REPL worldline."
  (interactive)
  (aura-lang-repl--command ":worldline"))

(defun aura-lang-repl-snapshot (&optional label)
  "Take a REPL snapshot, optionally named LABEL."
  (interactive "sSnapshot label (empty for none): ")
  (aura-lang-repl--command ":snapshot" (unless (string-empty-p label) label)))

(defun aura-lang-repl-restore (id)
  "Restore the REPL to snapshot ID."
  (interactive "sSnapshot ID: ")
  (aura-lang-repl--command ":restore" id))

(defun aura-lang-repl-src (name)
  "Show the live source of NAME."
  (interactive (list (or (thing-at-point 'symbol t) (read-string "Name: "))))
  (aura-lang-repl--command ":src" name))

(defun aura-lang-repl-doc (name)
  "Show the documentation of the primitive NAME."
  (interactive (list (or (thing-at-point 'symbol t) (read-string "Primitive: "))))
  (aura-lang-repl--command ":doc" name))

(defun aura-lang-repl-type (expression)
  "Print the type of EXPRESSION."
  (interactive (list (or (thing-at-point 'sexp t) (read-string "Expression: "))))
  (aura-lang-repl--command ":type" expression))

(provide 'aura-lang)
;;; aura-lang.el ends here
