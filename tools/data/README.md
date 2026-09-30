# Dictionary source

`english-common.txt` is the 5,000-word public-domain list from Michael Wehar's
[Public-Domain-Word-Lists](https://github.com/MichaelWehar/Public-Domain-Word-Lists)
repository (`5000-more-common.txt`). The upstream project states that this
list is released into the public domain. The app's C asset builder filters it
to ASCII alphabetic words, uppercases entries, removes duplicates, and merges
it with DeadSpeak's original investigation vocabulary and multilingual
transliterations before producing `app/src/main/assets/words.bin`.
