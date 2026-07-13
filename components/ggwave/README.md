# Vendored ggwave

This directory vendors the MIT-licensed ggwave encoder from
[`ggerganov/ggwave`](https://github.com/ggerganov/ggwave), commit
`060aec73dd7123ccac200442f75bdc7369795ffe` (2026-04-16).

Mimiclaw builds the transmitter in mono-tone, TX-only mode so it can send
Telegram reply text over the GPIO16 passive buzzer. See `LICENSE` and
`src/reed-solomon/LICENSE` for the included upstream notices.
