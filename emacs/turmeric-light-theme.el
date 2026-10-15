;;; turmeric-light-theme.el --- Turmeric Light color theme  -*- lexical-binding: t; -*-

;; Author: Turmeric Project
;; Keywords: faces, theme
;; Version: 0.1.0

;;; Commentary:

;; Emacs port of the Monaco "turmeric-light" theme used in the Try Turmeric
;; web REPL (web/main.js).  "Light Spice Market" palette: the roles of
;; turmeric-dark -- amber keywords, teal types, violet builtins, coral
;; strings, sage numbers -- re-stepped for a warm paper background.  Every
;; syntax ink clears 4.5:1 on the background; comments and delimiters are
;; deliberately faint, as in turmeric-dark.
;;
;; Popups and the mode line sit a step DARKER than the buffer here, where
;; turmeric-dark lifts them a step lighter: on a light ground a popup only
;; separates from the text behind it by its own.
;;
;; Use it with:
;;
;;   (add-to-list 'custom-theme-load-path
;;                (expand-file-name "/path/to/turmeric/emacs"))
;;   (load-theme 'turmeric-light t)

;;; Code:

(deftheme turmeric-light
  "Light Spice Market -- ported from the Try Turmeric Monaco theme.")

(let ((bg          "#FBF8F2")
      (bg-alt      "#F3EDE2")
      (bg-widget   "#EFE8DC")
      (bg-indent   "#E3D9C8")
      (border      "#D5C8B3")
      (fg          "#2A2016")
      (fg-dim      "#6A5B4B")
      (fg-muted    "#978874")
      (comment     "#A39682")
      (amber       "#965500")   ; amber as TEXT
      (gold        "#945602")   ; gold as TEXT / stroke
      (gold-fill   "#D9921F")   ; gold as a FILL, under on-gold ink
      (on-gold     "#1C1408")
      (teal        "#127065")
      (violet      "#7748AE")
      (coral       "#AE3C26")
      (sage        "#456F20")
      (op          "#8A7D6C")
      (delim       "#9F927F")
      (sel         "#EFDDBD"))

  (custom-theme-set-faces
   'turmeric-light

   ;; ---- Frame / editor chrome ----
   `(default                       ((t (:background ,bg :foreground ,fg))))
   `(cursor                        ((t (:background ,gold))))
   `(region                        ((t (:background ,sel :extend t))))
   `(secondary-selection           ((t (:background ,bg-widget))))
   `(highlight                     ((t (:background ,bg-alt))))
   `(hl-line                       ((t (:background ,bg-alt :extend t))))
   `(fringe                        ((t (:background ,bg :foreground ,fg-muted))))
   `(vertical-border               ((t (:foreground ,border))))
   `(window-divider                ((t (:foreground ,border))))
   `(window-divider-first-pixel    ((t (:foreground ,border))))
   `(window-divider-last-pixel     ((t (:foreground ,border))))
   `(line-number                   ((t (:background ,bg :foreground ,fg-muted))))
   `(line-number-current-line      ((t (:background ,bg-alt :foreground ,fg-dim :weight bold))))
   `(linum                         ((t (:background ,bg :foreground ,fg-muted))))
   `(mode-line                     ((t (:background ,bg-widget :foreground ,fg :box nil))))
   `(mode-line-inactive            ((t (:background ,bg-alt :foreground ,fg-dim :box nil))))
   `(mode-line-buffer-id           ((t (:foreground ,amber :weight bold))))
   `(mode-line-emphasis            ((t (:foreground ,amber))))
   `(header-line                   ((t (:background ,bg-widget :foreground ,fg))))
   `(minibuffer-prompt             ((t (:foreground ,amber :weight bold))))
   `(error                         ((t (:foreground ,coral :weight bold))))
   `(warning                       ((t (:foreground ,amber))))
   `(success                       ((t (:foreground ,sage))))
   `(link                          ((t (:foreground ,teal :underline t))))
   `(link-visited                  ((t (:foreground ,violet :underline t))))
   `(match                         ((t (:background ,gold-fill :foreground ,on-gold))))
   `(isearch                       ((t (:background ,gold-fill :foreground ,on-gold :weight bold))))
   `(lazy-highlight                ((t (:background ,sel :foreground ,fg))))
   `(show-paren-match              ((t (:background "#F0DDB8" :foreground ,amber :weight bold))))
   `(show-paren-mismatch           ((t (:background ,coral :foreground ,bg :weight bold))))
   `(trailing-whitespace           ((t (:background ,coral))))
   `(whitespace-space              ((t (:foreground ,bg-indent))))
   `(whitespace-tab                ((t (:foreground ,bg-indent))))
   `(fill-column-indicator         ((t (:foreground ,bg-indent))))

   ;; ---- Font-lock (lisp-mode + general) ----
   ;; Comments
   `(font-lock-comment-face            ((t (:foreground ,comment :slant italic))))
   `(font-lock-comment-delimiter-face  ((t (:foreground ,comment :slant italic))))
   `(font-lock-doc-face                ((t (:foreground ,fg-dim :slant italic))))
   ;; Strings
   `(font-lock-string-face             ((t (:foreground ,coral))))
   `(font-lock-regexp-grouping-construct ((t (:foreground ,amber))))
   `(font-lock-regexp-grouping-backslash ((t (:foreground ,amber))))
   ;; Numbers / constants -- sage
   `(font-lock-constant-face           ((t (:foreground ,sage))))
   `(font-lock-number-face             ((t (:foreground ,sage))))
   `(font-lock-negation-char-face      ((t (:foreground ,amber))))
   ;; Keywords -- amber bold (defn, if, let, ...)
   `(font-lock-keyword-face            ((t (:foreground ,amber :weight bold))))
   ;; Builtins / preprocessor -- violet (println, map, perform, ...)
   `(font-lock-builtin-face            ((t (:foreground ,violet))))
   `(font-lock-preprocessor-face       ((t (:foreground ,violet))))
   ;; Types -- teal
   `(font-lock-type-face               ((t (:foreground ,teal))))
   ;; Function definition names -- violet
   `(font-lock-function-name-face      ((t (:foreground ,violet))))
   `(font-lock-function-call-face      ((t (:foreground ,violet))))
   ;; Variable bindings -- base fg
   `(font-lock-variable-name-face      ((t (:foreground ,fg))))
   `(font-lock-variable-use-face       ((t (:foreground ,fg))))
   ;; Operators / delimiters (muted browns)
   `(font-lock-operator-face           ((t (:foreground ,op))))
   `(font-lock-delimiter-face          ((t (:foreground ,delim))))
   `(font-lock-bracket-face            ((t (:foreground ,delim))))
   `(font-lock-punctuation-face        ((t (:foreground ,delim))))
   `(font-lock-property-name-face      ((t (:foreground ,sage))))
   `(font-lock-property-use-face       ((t (:foreground ,sage))))
   ;; Warnings
   `(font-lock-warning-face            ((t (:foreground ,coral :weight bold))))

   ;; ---- Completion / company / corfu / vertico ----
   `(completions-common-part           ((t (:foreground ,amber :weight bold))))
   `(completions-first-difference      ((t (:foreground ,amber))))
   `(company-tooltip                   ((t (:background ,bg-widget :foreground ,fg))))
   `(company-tooltip-selection         ((t (:background "#E5D6BC" :foreground ,amber :weight bold))))
   `(company-tooltip-common            ((t (:foreground ,amber :weight bold))))
   `(company-tooltip-annotation        ((t (:foreground ,teal))))
   `(company-scrollbar-bg              ((t (:background ,bg-widget))))
   `(company-scrollbar-fg              ((t (:background "#C0B099"))))
   `(corfu-default                     ((t (:background ,bg-widget :foreground ,fg))))
   `(corfu-current                     ((t (:background "#E5D6BC" :foreground ,amber :weight bold))))
   `(corfu-border                      ((t (:background ,border))))
   `(vertico-current                   ((t (:background ,bg-alt :foreground ,amber :weight bold))))
   `(orderless-match-face-0            ((t (:foreground ,amber :weight bold))))
   `(orderless-match-face-1            ((t (:foreground ,teal))))
   `(orderless-match-face-2            ((t (:foreground ,violet))))
   `(orderless-match-face-3            ((t (:foreground ,sage))))

   ;; ---- Diff / magit / vc ----
   `(diff-added                        ((t (:foreground ,sage :background "#E6EFD9"))))
   `(diff-removed                      ((t (:foreground ,coral :background "#F6E1DA"))))
   `(diff-changed                      ((t (:foreground ,amber :background "#F1EBDD"))))
   `(diff-context                      ((t (:foreground ,fg-dim))))
   `(diff-header                       ((t (:background ,bg-widget :foreground ,fg))))
   `(diff-file-header                  ((t (:background ,bg-widget :foreground ,amber :weight bold))))
   `(magit-section-heading             ((t (:foreground ,amber :weight bold))))
   `(magit-branch-local                ((t (:foreground ,teal))))
   `(magit-branch-remote               ((t (:foreground ,violet))))
   `(magit-diff-added                  ((t (:foreground ,sage :background "#E6EFD9"))))
   `(magit-diff-removed                ((t (:foreground ,coral :background "#F6E1DA"))))
   `(magit-diff-context                ((t (:foreground ,fg-dim))))

   ;; ---- Flymake / flycheck ----
   `(flymake-error                     ((t (:underline (:style wave :color ,coral)))))
   `(flymake-warning                   ((t (:underline (:style wave :color ,amber)))))
   `(flymake-note                      ((t (:underline (:style wave :color ,teal)))))
   `(flycheck-error                    ((t (:underline (:style wave :color ,coral)))))
   `(flycheck-warning                  ((t (:underline (:style wave :color ,amber)))))
   `(flycheck-info                     ((t (:underline (:style wave :color ,teal)))))

   ;; ---- Org ----
   `(org-level-1                       ((t (:foreground ,amber :weight bold))))
   `(org-level-2                       ((t (:foreground ,teal :weight bold))))
   `(org-level-3                       ((t (:foreground ,violet :weight bold))))
   `(org-level-4                       ((t (:foreground ,sage))))
   `(org-document-title                ((t (:foreground ,amber :weight bold :height 1.2))))
   `(org-code                          ((t (:foreground ,coral))))
   `(org-block                         ((t (:background ,bg-alt :extend t))))
   `(org-block-begin-line              ((t (:foreground ,fg-dim :background ,bg-alt))))
   `(org-block-end-line                ((t (:foreground ,fg-dim :background ,bg-alt))))
   `(org-todo                          ((t (:foreground ,amber :weight bold))))
   `(org-done                          ((t (:foreground ,sage :weight bold))))
   `(org-link                          ((t (:foreground ,teal :underline t))))

   ;; ---- Tree-sitter (Emacs 29+) ----
   `(treesit-fontification-level-1     ((t (:foreground ,fg))))
   `(treesit-fontification-level-2     ((t (:foreground ,amber :weight bold))))
   `(treesit-fontification-level-3     ((t (:foreground ,violet))))
   `(treesit-fontification-level-4     ((t (:foreground ,teal))))

   ;; ---- ANSI terminal colors (vterm, ansi-color) ----
   ;; A light terminal inverts black and white: "black" is ink, "white" a
   ;; gray that still reads on paper.
   `(ansi-color-black                  ((t (:background ,fg :foreground ,fg))))
   `(ansi-color-red                    ((t (:foreground ,coral))))
   `(ansi-color-green                  ((t (:foreground ,sage))))
   `(ansi-color-yellow                 ((t (:foreground ,amber))))
   `(ansi-color-blue                   ((t (:foreground ,teal))))
   `(ansi-color-magenta                ((t (:foreground ,violet))))
   `(ansi-color-cyan                   ((t (:foreground ,teal))))
   `(ansi-color-white                  ((t (:foreground ,fg-dim))))))

(custom-theme-set-variables
 'turmeric-light
 `(ansi-color-names-vector
   ["#2A2016" "#AE3C26" "#456F20" "#965500"
    "#127065" "#7748AE" "#127065" "#6A5B4B"]))

;;;###autoload
(when (and (boundp 'custom-theme-load-path) load-file-name)
  (add-to-list 'custom-theme-load-path
               (file-name-directory load-file-name)))

(provide-theme 'turmeric-light)

;;; turmeric-light-theme.el ends here
