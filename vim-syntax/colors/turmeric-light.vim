" Vim colorscheme: turmeric-light
" Extracted from the Monaco "turmeric-light" theme used in the Try Turmeric
" web REPL (web/main.js). "Light Spice Market" palette: the dark theme's
" roles -- amber keywords, teal types, violet builtins, coral strings, sage
" numbers -- re-stepped for warm paper. Every syntax ink clears 4.5:1 on the
" background; comments and delimiters are deliberately faint, as in
" turmeric-dark.
"
" Requires a true-color terminal. Add to ~/.vimrc:
"   set termguicolors
"   colorscheme turmeric-light

hi clear
if exists("syntax_on")
  syntax reset
endif
let g:colors_name = "turmeric-light"
set background=light

" ---- Palette ----
"   bg          #FBF8F2    fg          #2A2016
"   amber       #965500    gold        #945602    gold-fill  #D9921F
"   teal        #127065    violet      #7748AE
"   coral       #AE3C26    sage        #456F20
"   comment     #A39682    line-bg     #F3EDE2
"   line-nr     #978874    line-nr-act #6A5B4B
"   op          #8A7D6C    delim       #9F927F
"   widget-bg   #EFE8DC    widget-bd   #D5C8B3
"
" Popups and bars sit a step DARKER than the buffer here, where turmeric-dark
" lifts them a step lighter: a terminal draws no shadow, so a popup only
" separates from the text behind it by its ground.

" ---- Editor chrome ----
hi Normal           guifg=#2A2016 guibg=#FBF8F2 ctermfg=235 ctermbg=255
hi NormalNC         guifg=#2A2016 guibg=#FBF8F2
hi Cursor           guifg=#FBF8F2 guibg=#945602
hi CursorLine                     guibg=#F3EDE2 cterm=NONE
hi CursorLineNr     guifg=#6A5B4B guibg=#F3EDE2 gui=bold
hi LineNr           guifg=#978874 guibg=#FBF8F2
hi SignColumn                     guibg=#FBF8F2
hi ColorColumn                    guibg=#F3EDE2
hi VertSplit        guifg=#D5C8B3 guibg=#FBF8F2
hi WinSeparator     guifg=#D5C8B3 guibg=#FBF8F2
hi StatusLine       guifg=#2A2016 guibg=#EFE8DC gui=NONE
hi StatusLineNC     guifg=#6A5B4B guibg=#F5F0E7 gui=NONE
hi TabLine          guifg=#6A5B4B guibg=#F5F0E7 gui=NONE
hi TabLineSel       guifg=#804900 guibg=#EFE8DC gui=bold
hi TabLineFill                    guibg=#FBF8F2
hi Folded           guifg=#6A5B4B guibg=#F3EDE2
hi FoldColumn       guifg=#A39682 guibg=#FBF8F2
hi MatchParen       guifg=#804900 guibg=#F0DDB8 gui=bold

" ---- Selection / search / diff ----
hi Visual                         guibg=#EFDDBD
hi Search           guifg=#1C1408 guibg=#E6A43A
hi IncSearch        guifg=#1C1408 guibg=#D9921F gui=bold
hi CurSearch        guifg=#1C1408 guibg=#D9921F gui=bold
hi DiffAdd          guifg=#456F20 guibg=#E6EFD9
hi DiffChange                     guibg=#F1EBDD
hi DiffDelete       guifg=#AE3C26 guibg=#F6E1DA
hi DiffText         guifg=#804900 guibg=#F0DDB8 gui=bold

" ---- Messages / popup widgets ----
hi Pmenu            guifg=#2A2016 guibg=#EFE8DC
hi PmenuSel         guifg=#804900 guibg=#E5D6BC gui=bold
hi PmenuSbar                      guibg=#EFE8DC
hi PmenuThumb                     guibg=#C0B099
hi WildMenu         guifg=#1C1408 guibg=#D9921F
hi NormalFloat      guifg=#2A2016 guibg=#EFE8DC
hi FloatBorder      guifg=#D5C8B3 guibg=#EFE8DC
hi ErrorMsg         guifg=#AE3C26 guibg=NONE gui=bold
hi WarningMsg       guifg=#965500 guibg=NONE
hi MoreMsg          guifg=#127065 guibg=NONE
hi Question         guifg=#127065 guibg=NONE
hi Title            guifg=#804900 guibg=NONE gui=bold
hi Directory        guifg=#127065 guibg=NONE

" ---- Spelling ----
hi SpellBad         guisp=#AE3C26 gui=undercurl
hi SpellCap         guisp=#965500 gui=undercurl
hi SpellRare        guisp=#7748AE gui=undercurl
hi SpellLocal       guisp=#127065 gui=undercurl

" ---- Syntax groups (the Monaco token -> Vim group mapping) ----
" Monaco "comment" -> Comment
hi Comment          guifg=#A39682 guibg=NONE gui=italic
hi SpecialComment   guifg=#6A5B4B guibg=NONE gui=italic
hi Todo             guifg=#1C1408 guibg=#D9921F gui=bold

" Monaco "string" -> String
hi String           guifg=#AE3C26 guibg=NONE
hi Character        guifg=#AE3C26 guibg=NONE
hi SpecialChar      guifg=#965500 guibg=NONE

" Monaco "number"/"number.float" -> Number/Float
hi Number           guifg=#456F20 guibg=NONE
hi Float            guifg=#456F20 guibg=NONE
hi Boolean          guifg=#456F20 guibg=NONE

" Monaco "keyword" -> Statement/Keyword/Conditional (amber, bold)
hi Statement        guifg=#965500 guibg=NONE gui=bold
hi Conditional      guifg=#965500 guibg=NONE gui=bold
hi Repeat           guifg=#965500 guibg=NONE gui=bold
hi Label            guifg=#965500 guibg=NONE gui=bold
hi Keyword          guifg=#965500 guibg=NONE gui=bold
hi Exception        guifg=#965500 guibg=NONE gui=bold
hi Define           guifg=#965500 guibg=NONE gui=bold

" Monaco "keyword.operator" -> builtins / Special (violet)
hi Function         guifg=#7748AE guibg=NONE
hi Special          guifg=#7748AE guibg=NONE
hi Macro            guifg=#7748AE guibg=NONE
hi PreProc          guifg=#7748AE guibg=NONE
hi PreCondit        guifg=#7748AE guibg=NONE
hi Include          guifg=#7748AE guibg=NONE

" Monaco "type" -> Type (teal)
hi Type             guifg=#127065 guibg=NONE
hi StorageClass     guifg=#127065 guibg=NONE
hi Structure        guifg=#127065 guibg=NONE
hi Typedef          guifg=#127065 guibg=NONE

" Monaco "operator" / "delimiter" (muted browns)
hi Operator         guifg=#8A7D6C guibg=NONE
hi Delimiter        guifg=#9F927F guibg=NONE

" Monaco "identifier" -> base fg
hi Identifier       guifg=#2A2016 guibg=NONE
hi Constant         guifg=#7748AE guibg=NONE

hi Error            guifg=#AE3C26 guibg=NONE gui=bold
hi NonText          guifg=#D5C8B3 guibg=NONE
hi EndOfBuffer      guifg=#FBF8F2 guibg=#FBF8F2
hi Whitespace       guifg=#E3D9C8 guibg=NONE
hi SpecialKey       guifg=#A39682 guibg=NONE
hi IndentBlanklineChar guifg=#E3D9C8 guibg=NONE

" ---- Direct mappings for the turmeric.vim syntax groups ----
" (Mirror the Monaco token roles exactly; identical to turmeric-dark.)
hi link turmericLineComment   Comment
hi link turmericDocComment    SpecialComment
hi link turmericBlockComment  Comment
hi link turmericString        String
hi link turmericStringEsc     SpecialChar
hi link turmericBadEsc        Error
hi link turmericCBlock        PreProc
hi link turmericHex           Number
hi link turmericBin           Number
hi link turmericFloat         Float
hi link turmericInt           Number
hi link turmericBool          Boolean
hi link turmericChar          Character
hi link turmericNil           Constant
hi link turmericKeyLit        Constant
hi link turmericMeta          PreProc
hi link turmericDefine        Define
hi link turmericControl       Conditional
hi link turmericType          Type
hi link turmericEffect        Keyword
hi link turmericExcept        Exception
hi link turmericNeotericCall  Function
hi link turmericDelim         Delimiter
hi link turmericCurlyInfix    Special
hi link turmericLangDir       PreProc
hi link turmericLangName      Type
