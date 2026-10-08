# Monitora_DG - Procedimentos de teste

Observaveis pelo Serial (linha `DIAG ...` a cada 60 s e mensagens de boot/transicao),
pelo ThingsBoard (telemetria/atributos) e pelo Telegram.

Campos do `DIAG`: `up` (uptime s), `wifi`, `rssi`, `ntp`, `rede`, `pend` (n pendentes TB),
`head`, `gap` (tbGapCount), `tgPend`, `tgNotif`, `tgForce`, `tgPhase`, `fallId`, `retId`,
`visId` (id da atual mensagem visual), `visDelId` (id aguardando exclusao, 0 = nenhum).
Na linha `BOOT DIAG` adicional: `bootId` (id da atual mensagem de reinicio), `bootDelId` (id de
reinicio aguardando exclusao, 0 = nenhum), `reason` (motivo do boot, igual ao da mensagem).

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

## L. Estado visual (4 LEDs) - substituicao de mensagem

1. Boot: apos o texto de boot, e enviada a linha visual. Se havia uma linha do boot
   anterior, ela e apagada (existe sempre **uma** so).
2. Provocar uma mudanca dos 4 sinais: nova linha enviada; a anterior apagada. Confirmar
   `visId` atualizado e `visDelId` = 0 apos a exclusao.
3. Mudanca rapida (offgrid ON -> ambos OFF -> gerador ON): coalescing de 3 s => uma unica
   linha com o estado final.
4. Durante FALTA/RETORNO (`tgPhase != NONE`): nenhuma linha visual e inserida.
5. Forcar falha de `deleteMessage()` da linha visual: `visDelId` permanece != 0; nenhuma
   nova linha visual e enviada enquanto pendente; mudancas continuam coalescidas. Ao
   restaurar a rede, a exclusao e confirmada (`visDelId` = 0) e a linha atual pendente e
   enviada.
6. Reboot com `visDelId` != 0: a exclusao pendente e retomada apos o boot, antes de enviar
   a nova linha visual de boot.
## Rodada corretiva - testes especificos

### Teste 1 - Payload historico do ThingsBoard

1. Com NTP sincronizado, provoque uma transicao qualquer.
2. Observe o payload publicado em `v1/devices/me/telemetry`.
3. Confirmar que o formato e:

```json
{
    "ts": 1700000000000,
    "values": {
        "rede_disponivel": 1,
        "alimentacao_rede": 0,
        "alimentacao_offgrid": 1,
        "alimentacao_gerador": 0
    }
}
```

4. `ts` no nivel externo, em **milissegundos**.
5. O `ts` corresponde ao momento do **evento**, nao ao momento da transmissao
   (teste com o TB off-line: reconecte e confirme que o `ts` e o antigo).

### Teste 2 - LostSession

1. Gerar um evento antes do NTP (`SessionMillis`).
2. Reiniciar antes da sincronizacao.
3. No boot, confirmar que o registro vira `LostSession` (sem conversao para epoch).
4. Ao publicar, o payload contem `"ts_unknown": true` e **nao** contem `"ts"`.

### Teste 3 - Caso C com fase antiga

1. Forcar um `state.bin` representando `redeDisponivel = NAO` com
   `tgPhase = TG_PHASE_DEL_OLD` (fase antiga de FALTA) e `tgPending` verdadeiro.
2. Iniciar com `redeDisponivel = SIM` no fisico.
3. Confirmar: caso C reconhecido; `tgForce` = 1; a fase antiga de FALTA nao e
   executada; a sequencia e `SEND_RET` e depois `TEXT_RET`; `tgNotifiedRede` nao
   suprime o RETORNO obrigatorio.

### Teste 4 - Falha no delete dos stickers

1. Forcar falha de `deleteMessage()` (ex.: derrubar a rede durante `DEL_OLD`).
2. Confirmar: `fallId`/`retId` continuam persistidos; `tgPhase` continua `del_old`;
   nao avanca para `send_fall`; o retry ocorre na proxima tentativa.
3. Restaurar a rede e confirmar: o ID e zerado e a fase avanca para `send_fall`.

### Teste 5 - Falha de montagem da LittleFS

1. Simular falha de montagem (FS corrompido / particao invalida).
2. Confirmar: **nao** ocorre `LittleFS.format()`; `state.bin` e `pending.bin`
   permanecem intactos; o Serial registra `ERRO: falha ao montar LittleFS`.

### Teste 6 - Alive (escrita unica)

1. Inspecionar `updateAlive()` em `src/main.cpp`.
2. Confirmar que existe **uma unica** chamada `saveState()` por atualizacao de alive.

### Teste 7 - Tamanho do PendingRecord

1. `static_assert(sizeof(PendingRecord) == 16, ...)` presente em `include/types.h`
   (`Timestamp` 8 B + `PowerState` 4 B + `rebootReason` 1 B + 3 B de alinhamento).
2. `pio run` compila com SUCCESS (o static_assert e verificado na compilacao).
3. Estimativa de capacidade: 16 B/registro => ~32.000 registros em 512 KiB (mais 4 B de
   cabecalho de formato), reduzida pelo novo campo `rebootReason` e aceita.

## Rodada - motivo do reinicio no ThingsBoard e retencao do aviso de boot

### Teste R1 - Cada boot publica o codigo do motivo (`reboot_reason`)

1. Reinicie o modulo por power-on, por `/reboot` e por watchdog, com NTP disponivel.
2. Em `v1/devices/me/telemetry`, confirmar **um** payload por boot com as quatro chaves de
   estado e adicionalmente `reboot_reason` **numerico**, conforme a tabela do README:
   power-on = 1, `/reboot` = 5, watchdog = 4 (software) ou 2 (hardware), OTA = 9.
3. No Telegram, confirmar que a mensagem de boot mostra a descricao em portugues correspondente
   (ex.: `Motivo: Reinício por software`). O Serial (`BOOT DIAG`: `reason=`) usa a mesma tabela.

### Teste R2 - Dois reboots consecutivos com o mesmo motivo

1. Reinicie duas vezes por `/reboot`.
2. Confirmar **dois** payloads distintos, ambos com `reboot_reason = 5`, cada um com o
   timestamp do respectivo boot.

### Teste R3 - Boot com Wi-Fi/ThingsBoard indisponivel

1. Reinicie sem Wi-Fi (ou com o TB fora).
2. Confirmar que `pend` cresce com o snapshot de boot (a publicacao fica em `pending.bin`).
3. Restaurar a conectividade: o snapshot e publicado com o **codigo original** do motivo e o
   **timestamp original** do boot, em **ordem cronologica**. Nada e descartado em silencio.

### Teste R4 - Transicoes normais sem `reboot_reason`

1. Provoque transicoes dos 4 sinais.
2. Confirmar que os payloads de transicao continuam **sem** `reboot_reason` (formato do
   Teste 1) e que o widget 2 continua exibindo somente as 4 series de estado.

### Teste R5 - Migracao do `pending.bin` legado

1. Gravar um `pending.bin` no formato antigo (12 B/registro, sem cabecalho).
2. Iniciar o firmware novo. Confirmar no Serial:
   `pending.bin: formato antigo (12B) migrado para 16B; historico preservado.`
3. Confirmar que **todos** os registros antigos foram preservados (`pend` igual ao anterior) e
   publicados em ordem, **sem** `reboot_reason`.

### Teste R6 - Retencao da mensagem de reinicio no Telegram

1. Boot 1 (sem mensagem anterior): nova mensagem enviada; `bootId` != 0; `bootDelId` = 0. No
   Serial: `TG: sendMessage chat_id=... result.message_id=N` e nenhum `deleteMessage`.
2. Boot 2: nova mensagem enviada; `bootId` atualizado; a mensagem do boot 1 e apagada. No Serial:
   `TG: deleteMessage envio ... message_id=<antigo>` e `deleteMessage resposta ok=1 error_code=0`;
   `bootDelId` volta a 0. No Telegram existe **uma** mensagem de reinicio.
3. Envio da nova mensagem falha (rede cai durante o `sendMessage`): a mensagem anterior
   **nao** e apagada; `bootId`/`bootDelId` inalterados; o envio e repetido depois.
4. Envio OK mas `deleteMessage()` recusado (`{"ok":false,...}`): no Serial
   `deleteMessage resposta ok=0 error_code=...`; persistir `bootDelId`; **nenhuma** nova mensagem
   de reinicio e criada enquanto pendente.
5. `deleteMessage` respondendo `message to delete not found`: no Serial
   `deleteMessage ja_inexistente id=...`; conta como sucesso (ja nao existe), mas se aparecer
   repetidamente indica que o id persistido nao e de uma mensagem viva (investigar).
6. Um HTTP 200 com `{"ok":false,...}` **nao** pode ser contado como sucesso (`request()` nao
   mascara mais; `deleteMessage` exige `"result":true`).
7. Reboot com `bootDelId` != 0: apos o boot, a exclusao pendente e resolvida **primeiro**;
   so depois a nova mensagem de reinicio e enviada.
8. Independencia: FALTA/RETORNO (stickers/textos) e a linha visual dos 4 LEDs nao sao afetados;
   `visId`/`visDelId` permanecem independentes de `bootId`/`bootDelId`; `sendSticker` tambem
   registra `chat_id`.
9. Confirmar no Serial que `saveState()` nunca registra `AVISO: falha ao gravar state.bin`
   (uma falha de gravacao perderia os IDs entre reboots e quebraria a retencao).

### Teste R7 - Retencao da linha visual (4 LEDs)

1. Boot: apos o texto de boot e enviada a linha visual; `visId` != 0; `visDelId` = 0.
2. Boot seguinte: nova linha; a anterior apagada (`visDelId` volta a 0). Existe **uma** linha.
3. `deleteMessage()` da linha recusado: `visDelId` persistido; nenhuma linha nova enquanto
   pendente; ao restaurar, a exclusao e confirmada e so entao a linha atual e enviada.
4. Uma mudanca dos 4 sinais gera nova linha e apaga a anterior (sem `AVISO: falha ao gravar state.bin`).

## Rodada de diagnostico - heap antes do Telegram

Objetivo: descobrir onde o maior bloco livre cai para ~12 KB, antes de a primeira
chamada TLS do Telegram precisar de 16709 bytes (`BR_SSL_BUFSIZE_MONO`).

Instrumentacao (somente leitura do estado do heap, sem alterar comportamento):
`include/heap_diag.h` -> `heapDiag(tag)` imprime, com prefixo `[millis()]`:
`HEAP: <tag> free=... maxblock=... frag=...%`.

Pontos registrados:
- setup: `setup:boot`, `setup:pos-littlefs`, `setup:pos-tbqueue`, `setup:pos-load`,
  `setup:pos-gpio`, `setup:pos-ntp-start`, `setup:pos-wifi-begin`,
  `setup:pos-telegram-begin`, `setup:pos-clients-begin`, `setup:pos-reconcile`, `setup:fim`;
- eventos: `evento:wifi-conectado` (borda), `evento:ntp-sincronizado`,
  `evento:mqtt-conectado` / `evento:mqtt-falha`;
- por requisicao Telegram (rodada anterior): `TG HEAP: <met> pre-new | pre-connect |
  pos-connect-fail | pos-close | pos-resposta` (a primeira ocorrencia e a primeira chamada).

Consumidores residentes relevantes (inspecao estatica):
- `src/telemetry.cpp`: `WiFiClientSecure tlsClient` **persistente** (MQTT/ThingsBoard) ->
  o BearSSL mantem ~16709 B (`_iobuf_in`) + 512 B (`_iobuf_out`) + contextos enquanto
  conectado; mais `PubSubClient mqtt` (buffer de 384 B) e `char payloadBuffer[256]` (BSS).
- `src/telegram.cpp`: cliente TLS **por chamada** (mesmo custo de ~16709 B, transitorio);
  `DynamicJsonDocument` 2048/1024 e 8192 (getUpdates) transitorios.
- `src/ota.cpp`: cliente TLS local (apenas durante OTA).
- `src/main.cpp`: objetos de namespace (BSS, pequenos).

Leitura esperada: o maior bloco deve cair de forma acentuada no `evento:mqtt-conectado`
(quando o BearSSL do ThingsBoard passa a manter o buffer residente), antes de qualquer
chamada do Telegram.
