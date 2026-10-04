# Monitora_DG - Procedimentos de teste

Observaveis pelo Serial (linha `DIAG ...` a cada 60 s e mensagens de boot/transicao),
pelo ThingsBoard (telemetria/atributos) e pelo Telegram.

Campos do `DIAG`: `up` (uptime s), `wifi`, `rssi`, `ntp`, `rede`, `pend` (n pendentes TB),
`head`, `gap` (tbGapCount), `tgPend`, `tgNotif`, `tgForce`, `tgPhase`, `fallId`, `retId`.

## A. Primeiro boot (sem state.bin)

1. Apague o FS (ou use um modulo novo). Ligue.
2. Esperado: `Sem estado persistente (primeira inicializacao).`; **nenhum** FALTA/RETORNO
   e nenhuma notificacao Telegram artificial; `state.bin` passa a existir.
3. Repita com `redeDisponivel` em SIM e em NAO.

## B. Reboot (carregue estado conhecido e reinicie)

| Antes | Depois | Esperado |
|---|---|---|
| SIM | SIM | nenhum evento; 1 snapshot de boot no TB |
| SIM | NAO | FALTA reconstruida (ts = alive local com rede=SIM) + snapshot; Telegram: FALTA |
| NAO | SIM | RETORNO reconstruido (ts atual, `tgForce`=1) + snapshot; Telegram: RETORNO mesmo se `tgNotif` ja coincide |
| NAO | NAO | nenhum evento; 1 snapshot de boot |

## C. Oscilacao com Telegram indisponivel

1. Bloqueie o acesso ao Telegram (mas mantenha Wi-Fi/TB).
2. Force SIM->NAO->SIM->NAO->SIM.
3. Esperado: o TB recebe **todos** os eventos (em ordem); ao liberar o Telegram, chega
   **uma unica** reconciliacao coerente com o estado final (nao cinco).

## D. Oscilacao com ThingsBoard indisponivel

1. Derrube o TB. Gere varios eventos e reinicie o ESP.
2. Esperado: `pend` cresce e persiste apos reboot; ao voltar o TB, os eventos sao enviados
   em ordem cronologica com os timestamps originais.

## E. Evento antes do NTP

1. Inicie sem internet; provoque uma transicao (gera `SessionMillis`).
2. Entregue NTP; esperado: `NTP sincronizado.` e `Log TB: timestamps ... convertidos para Epoch.`
   O evento e publicado com o epoch correspondente ao momento original.

## F. Evento antes do NTP + reboot

1. Provocar transicao antes do NTP, reiniciar antes de sincronizar.
2. Esperado: o registro vira `LostSession`; e transmitido com `ts_unknown: true`;
   **nunca** um epoch fabricado.

## G. Telegram - fases e reboot

1. FALTA completa; RETORNO completo; nova FALTA.
2. Em cada fase, reinicie o ESP e confirme que o `tgPhase` retoma a operacao.
3. Verifique `fallId`/`retId` persistidos apos cada envio.

## H. Stickers

- Apos FALTA: exatamente 1 sticker (Oliver).
- Apos RETORNO: exatamente 2 (Oliver + Huno).
- Proxima FALTA: os 2 anteriores apagados; apenas o novo FALTA.
- Textos nunca sao apagados.

## I. ThingsBoard

- Snapshot obrigatorio em todo boot.
- Snapshot em qualquer transicao dos 4 sinais.
- Historico em ordem; timestamps originais; eventos atrasados mantem o ts.

## J. Falta de espaco no pending.bin

- Induzir (temporariamente) um orcamento minusculo.
- Esperado: `AVISO: pending.bin sem espaco ...`; `gap` incrementa; GPIO continua; o estado
  atual continua sendo mantido/publicado. Sem corrupcao do log.

## K. Wi-Fi ausente

- Sem Wi-Fi: GPIO continua; transicoes continuam persistidas (`pend` cresce); ao voltar
  o Wi-Fi os eventos sao publicados.