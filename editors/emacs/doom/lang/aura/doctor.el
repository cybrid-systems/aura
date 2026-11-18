;; -*- lexical-binding: t; no-byte-compile: t; -*-
;;; lang/aura/doctor.el

;; aura-lang owns the executable lookup and the version parser; the module only
;; reads its answers.  Requiring it here keeps the doctor independent of whether
;; an Aura buffer happened to be opened in this session.
(require 'aura-lang nil t)

(defun +aura-doctor--state ()
  "Return the formatter gate's verdict as a plist.
Without aura-lang the binary is unknown, which is also the :missing state."
  (if (fboundp '+aura-formatter-state)
      (+aura-formatter-state)
    (list :state 'missing :version nil)))

(let* ((executable (and (fboundp 'aura-lang-executable)
                        (condition-case nil
                            (aura-lang-executable)
                          (error nil))))
       (state (+aura-doctor--state))
       (kind (plist-get state :state))
       (version (plist-get state :version)))
  (unless executable
    (warn! "Could not find the aura executable.  The REPL, LSP and formatting
are disabled.  Install Aura (install.sh links ~/.local/bin/aura), then run
'doom env' so the refreshed PATH snapshot reaches Emacs."))

  (when (eq kind 'unparsed)
    (warn! "'aura --version' printed %s, which carries no orderable version (a
source-tree \"aura dev (<sha>)\" build, for example), so no formatter is
registered.  If you know the build contains #5263, set
+aura-trust-nightly-formatter to t." version))

  (when (eq kind 'too-old)
    (warn! "aura %s predates the comment-preserving '--fmt' (#5263 wave 2, commit
c04a44c3e; first release 0.2.7).  No formatter is registered: '(format
+onsave)' will not run 'aura --fmt', so comments are safe but Aura files are
not formatted on save.  Upgrade Aura to 0.2.7 or later." version))

  (when (eq kind 'nightly)
    (warn! "aura %s is a nightly, and its commit cannot be ordered against
c04a44c3e from the version string, so no formatter is registered: save-time
formatting is off and comments are safe.  If you know the build contains
#5263, set +aura-trust-nightly-formatter to t."
           (or (plist-get state :commit) version))))

(when (modulep! +lsp)
  (unless (modulep! :tools lsp)
    (error! "+lsp needs the :tools lsp module; without it 'lsp!' is undefined and
Aura buffers get no diagnostics and no goto-definition.")))

;; too-old and nightly explain themselves above; this speaks only when the
;; gate could not ask at all (no binary, unparsable output).
(when (and (modulep! :editor format)
           (modulep! :editor format +onsave)
           (memq (plist-get (+aura-doctor--state) :state) '(missing unparsed)))
  (warn! "(format +onsave) is on but Aura has no formatter, so saving an Aura file
leaves it unformatted instead of risking comment loss (aura --fmt keeps
comments only from 0.2.7).  Use 'SPC c f' to format deliberately once the
binary is new enough."))
