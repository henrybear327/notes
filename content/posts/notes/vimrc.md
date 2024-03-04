---
title: 'My .vimrc'
date: 2024-03-04T23:04:19+01:00
tags: ["vimrc"]
categories: ["vim"]
author: ["Chun-Hung Tseng"]
draft: false
---

My simple `.vimrc` file which I am using for servers.

# `.vimrc`

Just copy the following text and paste it to `~/.vimrc`, open `vim`, and you are good to go! :) 

```vim
set number              " Show line numbers
set mouse=a             " Enable inaction via mouse
set showmatch           " Highlight matching brace
set cursorline          " Show underline
set cursorcolumn        " Highlight vertical column
set ruler               " Show row and column ruler information

filetype on             " Enable file detection
syntax on               " Syntax highlight

set expandtab           " For python code indentation to work correctly
set autoindent          " Auto-indent new lines
set shiftwidth=2        " Number of auto-indent spaces
set smartindent         " Enable smart-indent
set smarttab            " Enable smart-tabs
set tabstop=2           " Number of spaces per Tab

" ---------Optional----------
set undolevels=10000    " Number of undo levels
set scrolloff=5         " Auto scroll

set hlsearch            " Highlight all search results
set smartcase           " Enable smart-case search
set ignorecase          " Always case-insensitive
set incsearch           " Searches for strings incrementally

highlight Comment ctermfg=cyan
set showmode

set encoding=utf-8
set fileencoding=utf-8
scriptencoding=utf-8

set backspace=indent,eol,start
```
