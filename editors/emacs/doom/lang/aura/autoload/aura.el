;;; lang/aura/autoload/aura.el -*- lexical-binding: t; -*-

;; Doom loads module autoloads (including `doom doctor') before config.el, so
;; the formatter gate lives here: both the startup config and doctor need it.

;;
;;; The `aura --fmt' gate

;; #5263 made `aura --fmt' a comment-preserving reindent.  The second wave
;; landed on main as c04a44c3e (2026-11-18); the first release that contains it
;; is 0.2.7, the release this module ships in (CHANGELOG, "## 0.2.7").  Older
;; binaries deleted every comment while still exiting 0, so a save hook wired
;; to them silently destroyed code.
(defconst +aura-fmt-fix-commit "c04a44c3e"
  "Commit that made `aura --fmt' keep comments (#5263, wave 2).")

(defconst +aura-min-format-version "0.2.7"
  "First release whose `aura --fmt' keeps comments.
0.2.7 is the first tagged release after +aura-fmt-fix-commit; the fix is in
no earlier tag.")

(defcustom +aura-trust-nightly-formatter nil
  "Trust a `aura' whose version string carries no orderable version.
A nightly reports a commit hash (`aura nightly (cf7353373)') and a source
tree build reports aura dev (<sha>); neither can be ordered against
+aura-fmt-fix-commit without the git history, so the module cannot tell
whether the binary contains the comment-preserving `--fmt' (#5263).  Set
this to t only when you have checked that your build is from
+aura-fmt-fix-commit or later."
  :type 'boolean
  :group 'aura-lang)

;;;###autoload
(defun +aura-formatter-state ()
  "Return a plist describing whether the aura formatter may be registered.
:state is one of
  ok       the binary's `--fmt' keeps comments (register the formatter)
  too-old  a release before +aura-min-format-version
  nightly  a nightly, whose commit cannot be ordered from the version string
  unparsed `aura --version' printed something this module does not know
  missing  no usable aura executable"
  (let ((version (condition-case nil
                     (aura-lang-version)
                   (error nil))))
    (pcase (plist-get version :kind)
      ('release
       (if (version<= +aura-min-format-version (plist-get version :version))
           (list :state 'ok :version (plist-get version :version))
         (list :state 'too-old :version (plist-get version :version))))
      ('nightly
       (if +aura-trust-nightly-formatter
           (list :state 'ok :version (plist-get version :raw)
                 :commit (plist-get version :commit))
         (list :state 'nightly :version (plist-get version :raw)
               :commit (plist-get version :commit))))
      ('unknown
       (if +aura-trust-nightly-formatter
           (list :state 'ok :version (plist-get version :raw))
         (list :state 'unparsed :version (plist-get version :raw))))
      (_ (list :state 'missing :version nil)))))
