;;; lang/aura/config.el -*- lexical-binding: t; -*-

;; From the module's autoload file, Doom's :tools lsp, and imenu.
(declare-function +aura-formatter-state "aura" ())
(declare-function lsp! "lsp-mode" ())
(declare-function imenu--make-index-alist "imenu" (&optional noerror))

;; Load the gate even when Doom's autoload scan skipped this module.  See the
;; header of autoload/aura.el for why a symlinked module directory is skipped.
(let ((module-file (or load-file-name
                       (bound-and-true-p byte-compile-current-file)
                       buffer-file-name)))
  (when module-file
    (load (expand-file-name "autoload/aura.el" (file-name-directory module-file))
          nil 'nomessage)))

;;
;;; The `aura --fmt' gate

;; The gate lives in autoload/aura.el, loaded above.  `doom doctor' asks the
;; same question without loading config.el, so doctor.el loads that file too.
;; See `+aura-formatter-state'.

(defcustom +aura-repl-timeout 10.0
  "Seconds to wait for the Aura REPL to answer one input."
  :type 'number
  :group 'aura-lang)

(defcustom +aura-result-duration 8
  "Seconds the inline result overlay stays for an error.
Nil or 0 keeps it until the next result.  Only used with :tools eval
+overlay; non-error results use Doom's own overlay timing."
  :type '(choice (const :tag "Keep" nil) integer)
  :group 'aura-lang)

;;
;;; REPL plumbing

(defvar +aura--result-overlay nil
  "Overlay showing the last error the REPL reported.")

(defun +aura--wait-quiet (process)
  "Wait until PROCESS has printed nothing for a moment."
  (let ((size -1)
        (quiet 0))
    (while (< quiet 3)
      (accept-process-output process 0.1)
      (if (= size (buffer-size))
          (setq quiet (1+ quiet))
        (setq size (buffer-size)
              quiet 0)))))

(defconst +aura--repl-banner-patterns
  '("aura: repl preset="
    "AI-native Lisp"
    "(quit) to exit"
    ":help for REPL commands")
  "Lines the REPL prints before it accepts input (#5361).
The first send can capture them when the banner is flushed after the
process mark is recorded, so they are stripped from the answer.")

(defun +aura--banner-line-p (line)
  "Return non-nil when LINE is part of the REPL's startup banner."
  (seq-some (lambda (needle)
              (string-match-p (regexp-quote needle) line))
            +aura--repl-banner-patterns))

(defun +aura--repl-buffer ()
  "Return a live, ready Aura REPL buffer, starting one if necessary."
  (let* ((buffer (get-buffer aura-lang-repl-buffer))
         (live (and buffer (comint-check-proc buffer))))
    (unless live
      (setq buffer (run-aura))
      ;; The banner is printed before the REPL accepts input; sending into a
      ;; process that has not printed all of it yet interleaves the two.
      (with-current-buffer buffer
        (let ((process (get-buffer-process buffer))
              (deadline (+ (float-time) 3.0)))
          (while (and process
                      (not (save-excursion
                             (goto-char (point-max))
                             (search-backward ":help for REPL commands" nil t)))
                      (< (float-time) deadline))
            (accept-process-output process 0.05))
          (+aura--wait-quiet process))))
    buffer))

(defun +aura--strip-prompt (line)
  "Return LINE without the REPL's own prompt, if it has one."
  (let ((text (string-trim-left line)))
    (if (or (string-prefix-p "> " text)
            (string-prefix-p ".. " text))
        (substring text 2)
      line)))

(defun +aura--prompt-only-p (line)
  "Return non-nil when LINE is nothing but prompt characters."
  (let ((text (string-trim line)))
    (and (not (string-empty-p text))
         (seq-every-p (lambda (char) (memq char '(?. ?>))) text))))

(defun +aura--clean-output (raw input)
  "Strip the echoed INPUT and the REPL prompt from RAW.
The REPL prints its prompt as ordinary output (\"> \" or the continuation
\".. \") and comint keeps the input it sent at the head of the region, so
both have to go before the answer can be shown."
  (let ((out (mapcar #'+aura--strip-prompt (split-string raw "\n")))
        (echo (split-string (string-trim input) "\n")))
    ;; #5361: the first send can start after the process mark but before the
    ;; banner is flushed, which leaves the banner at the head of RAW.
    (while (and out (+aura--banner-line-p (car out)))
      (setq out (cdr out)))
    (while (and out echo
                (string= (string-trim (car out)) (string-trim (car echo))))
      (setq out (cdr out)
            echo (cdr echo)))
    (setq out (nreverse out))
    (while (and out
                (let ((line (string-trim (car out))))
                  (or (string-empty-p line)
                      (+aura--prompt-only-p line))))
      (setq out (cdr out)))
    (string-trim (mapconcat #'identity (nreverse out) "\n"))))

(defun +aura--send (text)
  "Send TEXT to the Aura REPL and return what it printed, or nil on timeout.
TEXT is one input, as `comint-send-input' would send it.  The REPL
evaluates a complete form per input; nothing is inserted in the source
buffer.  The answer ends when the process has been quiet for a moment:
aura repl prints a prompt, but its shape differs between the PTY path and
the pipe path, so the module does not key on it."
  (let* ((buffer (+aura--repl-buffer))
         (process (get-buffer-process buffer))
         (deadline (+ (float-time) +aura-repl-timeout))
         (start nil))
    (with-current-buffer buffer
      (setq start (copy-marker (or (process-mark process) (point-max))))
      (goto-char (point-max))
      (insert text)
      (unless (bolp) (insert "\n"))
      (comint-send-input nil t)
      (let ((size -1)
            (quiet 0))
        (while (and (< quiet 3) (< (float-time) deadline))
          (accept-process-output process 0.1)
          (if (= size (buffer-size))
              (setq quiet (1+ quiet))
            (setq size (buffer-size)
                  quiet 0))))
      (prog1 (unless (>= (float-time) deadline)
               (+aura--clean-output
                (buffer-substring-no-properties start (point-max))
                text))
        (set-marker start nil)
        (when (>= (float-time) deadline)
          (message "Aura REPL did not answer within %ss" +aura-repl-timeout))))))

(defun +aura--error-output-p (text)
  "Return non-nil when the REPL answered TEXT with an error.
The patterns are the REPL's own, from `aura-lang-repl-font-lock-keywords'."
  (seq-some (lambda (keyword) (string-match-p (car keyword) text))
            aura-lang-repl-font-lock-keywords))

(defun +aura--display-error (text)
  "Show TEXT as an error at the end of the current line."
  (when (overlayp +aura--result-overlay)
    (delete-overlay +aura--result-overlay))
  (setq +aura--result-overlay
        (let ((overlay (make-overlay (line-end-position) (line-end-position))))
          (overlay-put overlay 'after-string
                       (propertize (concat " ; => " text) 'face 'error))
          (when (and +aura-result-duration (> +aura-result-duration 0))
            (run-at-time +aura-result-duration nil
                         (lambda (buffer)
                           (when (buffer-live-p buffer)
                             (with-current-buffer buffer
                               (when (overlayp +aura--result-overlay)
                                 (delete-overlay +aura--result-overlay)
                                 (setq +aura--result-overlay nil)))))
                         (current-buffer)))
          overlay)))

(defun +aura--show-result (output)
  "Show OUTPUT next to the line at point, as Doom's :tools eval +overlay does."
  (let ((text (replace-regexp-in-string "[ \t]*\n[ \t]*" " ↵ "
                                       (string-trim output))))
    (when (string-empty-p text)
      (setq text "(no output)"))
    (when (> (length text) 200)
      (setq text (concat (substring text 0 200) "…")))
    (if (+aura--error-output-p output)
        (+aura--display-error text)
      (when (overlayp +aura--result-overlay)
        (delete-overlay +aura--result-overlay)
        (setq +aura--result-overlay nil))
      (if (and (modulep! :tools eval +overlay)
               (fboundp '+eval-display-results))
          (+eval-display-results text (current-buffer))
        (message "%s" text)))))

(defun +aura-eval-region (beg end)
  "Send BEG..END to the Aura REPL and show what it answered.
This is the :tools eval handler for `aura-mode'; it never evaluates in
process, so the REPL's workspace is the one that changes."
  (interactive "r")
  (let ((output (+aura--send (buffer-substring-no-properties beg end))))
    (when output (+aura--show-result output))
    output))

(defun +aura-send-defun ()
  "Send the definition at point to the Aura REPL."
  (interactive)
  (save-excursion
    (end-of-defun)
    (let ((end (point)))
      (beginning-of-defun)
      (+aura-eval-region (point) end))))

(defun +aura-send-region (beg end)
  "Send BEG..END to the Aura REPL."
  (interactive "r")
  (+aura-eval-region beg end))

(defun +aura-send-buffer ()
  "Send the whole buffer to the Aura REPL."
  (interactive)
  (+aura-eval-region (point-min) (point-max)))

;;
;;; Lookup

(defun +aura--lsp-client ()
  "Return the LSP client managing the current buffer, or nil.
eglot wins when both answer: under `(lsp +eglot)' its hover is what K has
to use, because lsp-mode's own K handler is not installed there."
  (cond ((and (fboundp 'eglot-managed-p) (eglot-managed-p)) 'eglot)
        ((bound-and-true-p lsp-mode) 'lsp-mode)
        (t nil)))

(defun +aura--show-documentation (id text)
  "Show ID and TEXT in *aura-doc* and return t."
  (let ((buffer (get-buffer-create "*aura-doc*")))
    (with-current-buffer buffer
      (let ((inhibit-read-only t))
        (erase-buffer)
        (insert (format "%s\n\n%s\n" id text)))
      (special-mode)
      (goto-char (point-min)))
    (display-buffer buffer)
    t))

(defun +aura--hover-documentation ()
  "Return the LSP hover text for the symbol at point, or nil.
eglot publishes hover as an eldoc function; call it and wait briefly for
the asynchronous answer.  lsp-mode displays its own describe buffer, so it
does not come through here."
  (when (and (eq (+aura--lsp-client) 'eglot)
             (fboundp 'eglot-hover-eldoc-function))
    (let ((text nil)
          (done nil))
      (condition-case nil
          (eglot-hover-eldoc-function
           (lambda (doc &rest _)
             (when (stringp doc) (setq text doc))
             (setq done t)))
        (error nil))
      (let ((deadline (+ (float-time) 1.0)))
        (while (and (not done) (< (float-time) deadline))
          (accept-process-output nil 0.05)))
      (when (and (stringp text) (not (string-empty-p (string-trim text))))
        text))))

(defun +aura--repl-documentation (id)
  "Show the REPL's `:doc' answer for ID in *aura-doc*."
  (let ((output (and id (+aura--send (concat ":doc " id)))))
    (when (and output (not (string-empty-p output)))
      (+aura--show-documentation id output))))

(defun +aura--lsp-hover-text ()
  "Return the LSP hover text for the symbol at point, or nil.
The server answers primitives from the registry its hover already uses,
so `g d' can decide without starting a REPL (#5357)."
  (pcase (+aura--lsp-client)
    ('eglot (+aura--hover-documentation))
    ('lsp-mode
     (when (and (fboundp 'lsp-request)
                (fboundp 'lsp--text-document-position-params))
       ;; #5368: Doom's :tools lsp builds and runs lsp-mode with
       ;; LSP_USE_PLISTS=1, so `lsp-request' answers plists there, not hash
       ;; tables.  `lsp-get' is lsp-mode's own mode-aware accessor; a
       ;; MarkupContent[] reply arrives as a vector (or list) of parts.
       (let* ((result (ignore-errors
                        (lsp-request
                         "textDocument/hover"
                         (lsp--text-document-position-params))))
              (contents (and result (lsp-get result :contents)))
              (text (cond ((stringp contents) contents)
                          ((or (hash-table-p contents)
                               (and (listp contents) (keywordp (car contents))))
                           (lsp-get contents :value))
                          ((or (vectorp contents) (listp contents))
                           (mapconcat
                            (lambda (part)
                              (cond ((stringp part) part)
                                    ((or (hash-table-p part)
                                         (and (listp part) (keywordp (car part))))
                                     (or (lsp-get part :value) ""))
                                    (t "")))
                            (append contents nil) "\n")))))
         (and (stringp text)
              (not (string-empty-p (string-trim text)))
              text))))))

(defun +aura--primitive-p (id)
  "Return non-nil when ID is a primitive, which has no source definition.
A primitive must not fall through to the project-wide search, which lands
on an unrelated use of the name (#5357).  With an LSP client the server's
own registry answers, so `g d' does not start a REPL; without one the
REPL's `:doc` is the only source."
  (let ((client (+aura--lsp-client)))
    (if (and client (equal id (thing-at-point 'symbol t)))
        (+aura--lsp-hover-text)
      (let ((answer (ignore-errors (+aura--send (concat ":doc " id)))))
        (and (stringp answer)
             (not (string-empty-p (string-trim answer)))
             (not (string-match-p "no such primitive\\|no description" answer)))))))

(defun +aura-lookup-definition (&optional identifier)
  "Jump to the definition of IDENTIFIER.
The LSP server answers through xref when it is running (lsp-mode or eglot);
without one this falls back to the buffer's imenu, which aura-mode fills
from its definition forms.  A primitive has no source definition, so it
stops the lookup instead of letting the generic fallback jump elsewhere."
  (interactive)
  (let ((id (or identifier (thing-at-point 'symbol t))))
    (unless id
      (user-error "No symbol at point"))
    (if (condition-case nil
            (progn (xref-find-definitions id) t)
          (error nil))
        t
      (let ((entry (assoc-string id (imenu--make-index-alist t))))
        (cond
         (entry (imenu entry) t)
         ((+aura--primitive-p id)
          ;; #5357: a `user-error' here is what broke `g d'.  Doom's
          ;; `+lookup--run-handlers' records any error as a failed handler and
          ;; moves on, so the project-search fallback jumped to an unrelated
          ;; use of the name.  Return non-nil to end the chain, and say why
          ;; nothing moved.
          (message "%s is a primitive (no source definition)" id)
          t)
         (t
          ;; Nothing in this buffer: let imenu ask for a name.
          (imenu)
          t))))))

(defun +aura-lookup-documentation (&optional identifier)
  "Show the documentation of IDENTIFIER.
With an LSP client on the buffer this uses the server's hover (eglot) or
lsp-mode's describe buffer; those carry the hover the server has served
since the symbol layer landed.  Without a client it asks the REPL, whose
`:doc' knows the primitives."
  (interactive)
  (let* ((id (or identifier (thing-at-point 'symbol t)))
         (client (+aura--lsp-client))
         (hover (and (eq client 'eglot) (+aura--hover-documentation))))
    (unless id
      (user-error "No symbol at point"))
    (cond
     (hover (+aura--show-documentation id hover))
     ((and (eq client 'lsp-mode) (fboundp 'lsp-describe-thing-at-point))
      (call-interactively #'lsp-describe-thing-at-point))
     (t (+aura--repl-documentation id)))))

;;
;;; Packages

(use-package! aura-lang
  :defer t
  :config
  (set-repl-handler! 'aura-mode #'run-aura :persist t)
  (set-eval-handler! 'aura-mode #'+aura-eval-region)
  (set-lookup-handlers! 'aura-mode
    :definition #'+aura-lookup-definition
    :documentation #'+aura-lookup-documentation)

  ;; The formatter is registered only when the binary's `--fmt' keeps
  ;; comments; see the gate at the top of this file and `doctor.el'.
  (when (eq 'ok (plist-get (+aura-formatter-state) :state))
    (set-formatter! '+aura-fmt
      (list (aura-lang-executable) "--fmt")
      :modes '(aura-mode)))

  (when (modulep! +lsp)
    ;; doom-run-local-var-hooks-h creates MODE-local-vars-hook on demand, so
    ;; this needs no definition here.
    (add-hook 'aura-mode-local-vars-hook #'lsp! 'append))

  (when (modulep! :ui popup)
    (set-popup-rule! "^\\*aura\\*$"
      :side 'bottom :size 0.3 :select t :quit nil)
    (set-popup-rule! "^\\*aura-doc\\*$"
      :side 'right :size 0.45 :select t :quit t :ttl 0))

  (when (modulep! :editor evil)
    ;; [m / ]m follow the same top-level sexp stops as C-M-a (#5482).
    ;; :nm is evil normal and motion.  Doom's own [m uses a column-0
    ;; search, which skips an indented form and a second form on a line.
    (map! :map aura-mode-map
          :nm "[m" #'beginning-of-defun
          :nm "]m" #'aura-lang-next-defun)
    (map! (:localleader
           :map aura-mode-map
           "'" #'run-aura
           "h" #'+aura-lookup-documentation
           "r" #'aura-lang-reload-file
           (:prefix ("g" . "goto")
            "d" #'+lookup/definition)
           (:prefix ("s" . "send")
            "d" #'+aura-send-defun
            "r" #'+aura-send-region
            "b" #'+aura-send-buffer)
           (:prefix ("w" . "world")
            "w" #'aura-lang-repl-worldline
            "s" #'aura-lang-repl-snapshot
            "r" #'aura-lang-repl-restore
            "k" #'aura-lang-repl-ws)))))
