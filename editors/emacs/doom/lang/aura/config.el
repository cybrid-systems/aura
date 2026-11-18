;;; lang/aura/config.el -*- lexical-binding: t; -*-

;; From the module's autoload file, Doom's :tools lsp, and imenu.
(declare-function +aura-formatter-state "aura" ())
(declare-function lsp! "lsp-mode" ())
(declare-function imenu--make-index-alist "imenu" (&optional noerror))

;;
;;; The `aura --fmt' gate

;; The gate lives in autoload/aura.el: `doom doctor' asks the same question
;; without loading config.el.  See `+aura-formatter-state'.

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

(defun +aura--repl-buffer ()
  "Return a live, ready Aura REPL buffer, starting one if necessary."
  (let* ((buffer (get-buffer aura-lang-repl-buffer))
         (live (and buffer (comint-check-proc buffer))))
    (unless live
      (setq buffer (run-aura))
      ;; The banner is printed before the REPL accepts input; sending into a
      ;; process that has not printed it yet interleaves the two.
      (with-current-buffer buffer
        (let ((deadline (+ (float-time) 3.0)))
          (while (and (zerop (buffer-size)) (< (float-time) deadline))
            (accept-process-output (get-buffer-process buffer) 0.05)))
        (+aura--wait-quiet (get-buffer-process buffer))))
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

(defun +aura-lookup-definition (&optional identifier)
  "Jump to the definition of IDENTIFIER.
The LSP server answers through xref when it is running (lsp-mode or eglot);
without one this falls back to the buffer's imenu, which aura-mode fills
from its definition forms."
  (interactive)
  (let ((id (or identifier (thing-at-point 'symbol t))))
    (unless id
      (user-error "No symbol at point"))
    (if (condition-case nil
            (progn (xref-find-definitions id) t)
          (error nil))
        t
      (let ((entry (assoc-string id (imenu--make-index-alist t))))
        (if entry
            (progn (imenu entry) t)
          ;; Nothing in this buffer: let imenu ask for a name.
          (imenu)
          t)))))

(defun +aura-lookup-documentation (&optional identifier)
  "Show the documentation of IDENTIFIER in *aura-doc*.
The server publishes no hover documentation yet (docs/editor.md), so this
asks the REPL, whose `:doc' knows the primitives, and shows the answer in
its own window."
  (interactive)
  (let* ((id (or identifier (thing-at-point 'symbol t)))
         (output (and id (+aura--send (concat ":doc " id)))))
    (when (and output (not (string-empty-p output)))
      (let ((buffer (get-buffer-create "*aura-doc*")))
        (with-current-buffer buffer
          (let ((inhibit-read-only t))
            (erase-buffer)
            (insert (format "%s\n\n%s\n" id output)))
          (special-mode)
          (goto-char (point-min)))
        (display-buffer buffer)
        t))))

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
