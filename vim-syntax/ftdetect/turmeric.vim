" Turmeric filetype detection.
"
" `.tur.sweet` must be tested BEFORE `.tur`: Vim matches the last extension, so
" a bare `*.tur` rule would never see a sweet-exp file, and the two share this
" filetype (the reader differs, the syntax does not).
autocmd BufRead,BufNewFile *.tur.sweet setfiletype turmeric
autocmd BufRead,BufNewFile *.tur       setfiletype turmeric
" A manifest is Turmeric source too, in either spelling.
autocmd BufRead,BufNewFile build.tur,build.tur.sweet setfiletype turmeric
