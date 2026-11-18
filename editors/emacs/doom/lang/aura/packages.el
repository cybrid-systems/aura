;; -*- no-byte-compile: t; -*-
;;; lang/aura/packages.el

;; aura-lang is this repository's own editors/emacs/aura-lang.el.  The recipe
;; pins the repository and the file set (only the package files are fetched,
;; not the engine), which is how a Doom module pins a package it does not get
;; from ELPA.  There is no :pin commit on purpose: the Elisp client, the LSP
;; handshake and the --fmt version gate all have to match the aura binary you
;; run, so lock the module and the binary to the same tag (v0.2.7 and later)
;; rather than freezing the Elisp side at a commit.
(package! aura-lang
  :recipe (:host github
           :repo "cybrid-systems/aura"
           :files ("editors/emacs/*.el")))
