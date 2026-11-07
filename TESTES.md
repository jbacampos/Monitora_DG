# Monitora_DG - Procedimentos de teste

Observaveis pelo Serial (linha `DIAG ...` a cada 60 s e mensagens de boot/transicao),
pelo ThingsBoard (telemetria/atributos) e pelo Telegram.

Campos do `DIAG`: `up` (uptime s), `wifi`, `rssi`, `ntp`, `rede`, `pend` (n pendentes TB),
`head`, `gap` (tbGapCount), `tgPend`, `tgNotif`, `tgForce`, `tgPhase`, `tgStuck` (fase sem
progresso ha mais de `TELEGRAM_PHASE_STUCK_MS`), `fallId`, `retId`,
`visId` (id da atual mensagem visual), `visDelId` (id aguardando exclusao, 0 = nenhum),
`vq` (fotografias na fila visual), `vqDrop` (coalescidas por overflow), `renew` (renovacao
silenciosa pendente), `fallM`/`retM`/`visM` (estado da exclusao preventiva: 0=none 1=waiting
2=done 3=expired).
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

## L. Estado visual (4 LEDs) - fila de transicoes e substituicao

1. Boot: apos o texto de boot, e enviada a linha visual (a fila persistida traz as transicoes
   confirmadas antes do reboot, seguidas do estado atual). Se havia uma linha do boot anterior,
   ela e apagada (existe sempre **uma** so).
2. Provocar uma mudanca dos 4 sinais: a transicao e enfileirada e a nova linha e enviada; a
   anterior apagada. Confirmar `visId` atualizado e `visDelId` = 0 apos a exclusao.
3. **FALTA -> RETORNO rapido (o caso de 05/11)**: as duas fotografias ficam na fila e sao enviadas
   em ordem (a de queda e depois a de retorno) assim que `tgPhase` volta a `none`. Confirmar `vq`
   decrementando de 2 ate 0 e **duas** linhas no grupo; nenhuma e descartada por o estado final
   coincidir com o anterior.
4. Mudanca real durante uma sequencia textual (`tgPhase != NONE`): a transicao e enfileirada e
   enviada ao final; a sequencia **adia**, nao descarta.
5. Uma queda dentro da janela de graca **nao** entra na fila (so a FALTA confirmada).
6. Forcar falha **transitoria** de `deleteMessage()` da linha visual: `visDelId` permanece != 0;
   nenhuma nova linha e enviada enquanto pendente; ao restaurar, a exclusao e confirmada e a linha
   pendente e enviada. (Recusa permanente — 48 h/sem direito: ver teste S1, o id e descartado.)
7. Reboot com transicoes na fila: apos o boot elas sao enviadas em ordem; `vq` volta a 0.
8. Reboot com `visDelId` != 0: a exclusao pendente e retomada apos o boot, antes de enviar a linha
   de boot.

## M. Exclusao preventiva (48 h)

1. Um `state.bin` migrado de 1.5.0 (ou anterior) carrega com os campos novos zerados: `fallM`,
   `retM` e `visM` = 0 e `vq` = 0; nenhuma exclusao e agendada para mensagens antigas. No Serial:
   `STATE: migrado de 84 para 156 bytes (estado anterior preservado).` Um `state.bin` com tamanho
   desconhecido e recusado: `ERRO: state.bin com tamanho desconhecido (...); ignorado (defaults).`
2. Bot administrador com `can_delete_messages`: gerar FALTA e RETORNO. A partir de ~60 min antes de
   48 h o Serial mostra as tentativas preventivas (`fallM`/`retM` -> 2 e o sticker some do grupo).
   O horario base e o `date` confirmado pela API (nao o inicio da tentativa).
3. Linha visual: ao apagar preventivamente, `visId` = 0 e `renew` = 1; em seguida uma **renovacao
   silenciosa** (`disable_notification=true`) e enviada, `visId` novo, `renew` = 0 e o ciclo de
   48 h recomeca (Serial: `ligar LEDs (renovacao silenciosa)`).
4. **Mudanca real durante a renovacao pendente**: a linha da transicao e enviada **com**
   notificacao (tem prioridade) e `renew` volta a 0; **nenhuma** renovacao redundante e enviada
   depois.
5. Internet fora durante toda a janela: `fallM`/`retM`/`visM` viram 3 (expired) e o Serial registra
   `janela de 48h vencida sem exclusao preventiva; desistindo`; nada fica bloqueado.
6. Reboot no meio da janela: o `nextAttempt` persistido e respeitado (o backoff sobrevive).
7. Durante toda a manutencao: `/reboot` e `/ota` continuam respondendo e as transicoes eletricas
   continuam sendo registradas (ThingsBoard) — roda no maximo uma acao por passagem e nunca
   enquanto `tgPhase != NONE`.

## Testes de logica no host (sem hardware)

`test/host/test_logic.cpp` cobre a parte pura do firmware (debounce de um canal, portao composto
do `InputGroup`, fracao de milissegundo do `Timestamp`, layout em Flash e diagnostico de
contradicao). Nao usa Arduino, rede nem LittleFS: roda no PC.

Compilar e executar a partir da raiz do repositorio:

```text
clang++ -std=c++17 -Wall -Wextra -Iinclude test/host/test_logic.cpp -o test/host/test_logic.exe
.\test\host\test_logic.exe
```

(g++/MinGW funcionam igual. `test/host` esta em `test_ignore` no `platformio.ini` porque e um
harness proprio com `main()`, nao uma suite Unity.)

Saida esperada: `62 verificacoes, 0 falha(s)` seguido de `OK: toda a logica pura passou.` e
codigo de saida 0 (o codigo de saida e o numero de falhas, limitado a 100).

### Teste H1 - transferencia escalonada (widget "Historico de Transicoes")

Cenario real: o ATS devolve a carga para a rede. O contator da rede fecha (`redeDisponivel`=1,
`alimentacaoRede`=1) e, ~200 ms depois, o contator do offgrid abre (`alimentacaoOffgrid`=0).

- Com a logica antiga (commit por canal confirmado) uma amostragem de 100 ms grava o composto
  `1/1/1` no intervalo - um estado que nunca existiu. O teste **reproduz** essa logica e exige
  que o artefato apareca (`legacySawImpossible`), provando que a regressao era real.
- Com o portao (commit so quando os quatro canais estao assentados) o composto impossivel nunca
  e produzido e a transferencia aparece uma unica vez, completa (`1/1/0`).

### Teste H2 - sobreposicao real nao e escondida

Se a sobreposicao durar mais que o debounce (contator emperrado, falha de fiacao), o estado e
reportado como medido e o Serial registra `AVISO: estado eletrico contraditorio ...`. O firmware
nunca corrige, atrasa nem descarta: o aviso existe para distinguir uma falha real de um artefato
de historico.

### Teste H3 - timestamps e compatibilidade

- `Epoch` com fracao de milissegundo: duas transicoes dentro do mesmo segundo mantem instantes
  distintos e a ordem correta; a conversao `SessionMillis -> Epoch` e exata de 0 a 5000 ms e
  idempotente (um `Epoch` nunca e deslocado duas vezes).
- Uma fracao invalida (registro gravado pela 1.4.0, quando esses bytes eram padding) e ignorada,
  nunca publicada.
- Guardas de layout: `sizeof(Timestamp)` = 8, `subsecond` no offset 2, `sizeof(PendingRecord)` =
  16 - `state.bin` e `pending.bin` gravados pela 1.4.0 continuam legiveis.

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

1. Forcar uma falha **transitoria** de `deleteMessage()` (ex.: derrubar a rede durante `DEL_OLD`).
2. Confirmar: `fallId`/`retId` continuam persistidos; `tgPhase` continua `del_old`;
   nao avanca para `send_fall`; o retry ocorre depois do backoff (`TG: retry fase=del_old ...`).
3. Restaurar a rede e confirmar: o ID e zerado e a fase avanca para `send_fall`.
4. Uma recusa **permanente** (`400 message can't be deleted` / `403`) e o caso do teste S1:
   o id e descartado e a sequencia continua.

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
4. Envio OK mas `deleteMessage()` recusado de forma **transitoria** (`{"ok":false,...}` com
   erro que nao seja 48 h/sem direito): no Serial
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
3. `deleteMessage()` da linha recusado de forma **transitoria**: `visDelId` persistido; nenhuma
   linha nova enquanto pendente; ao restaurar, a exclusao e confirmada e so entao a linha atual e
   enviada. (Recusa permanente — 48 h/sem direito: ver teste S1, o id e descartado.)
4. Uma mudanca dos 4 sinais gera nova linha e apaga a anterior (sem `AVISO: falha ao gravar state.bin`).

## Rodada - Telegram nao bloqueia mais os comandos

Contexto: a notificacao parou **e** o bot deixou de responder aos comandos, com o ThingsBoard
normal. Causa: `DEL_OLD` tentava apagar um sticker que a API recusa apagar para sempre —
`400 message can't be deleted` quando a mensagem tem mais de 48 h, `403` quando o bot nao e
administrador com o direito de apagar. Isso era tratado como falha transitoria, a fase ficava
persistida e `handleTelegramCommands()` deixava de chamar `getUpdates()`.

### Teste S1 - Exclusao impossivel (bot nao-admin ou mensagem com mais de 48 h)

1. Com o bot apenas **membro** do grupo (sem `can_delete_messages`), gere FALTA e depois RETORNO.
2. Na FALTA seguinte, o `DEL_OLD` tenta apagar stickers que a API recusa.
3. Esperado no Serial: `deleteMessage impossivel error_code=...` seguido de
   `DEL_FALL resultado=IMPOSSIVEL` / `DEL_RET resultado=IMPOSSIVEL` e
   `... descartado sem apagar (exclusao impossivel)`.
4. Esperado: o id e zerado, `tgPhase` avanca (`del_old` -> `send_fall` -> `text_fall`), a nova
   FALTA e enviada (`SEND_FALL OK`, `TEXT_FALL OK`) e `DIAG ... tgPhase=none`.
5. O sticker antigo **permanece** no grupo (a API nao permite apaga-lo); o firmware nao pode
   travar por isso. Promover o bot a administrador restabelece a substituicao normal.

### Teste S2 - Telegram inacessivel no boot

1. Bloqueie o acesso ao Telegram e reinicie o ESP.
2. Esperado: `aviso de boot` e a linha visual falham e sao repetidos com backoff
   (`TG: retry aviso de boot em=3000ms tentativa=1`, depois `6000`, `12000`, `24000`, `48000`,
   `60000`), sem uma requisicao por passagem do loop.
3. Restaure o acesso: os envios terminam sem mensagens duplicadas.
4. Envie `/reboot`: o comando responde e o `update_id` avanca (o comando nao se repete).

### Teste S3 - `/reboot` com fase travada (rede de seguranca)

1. Forcar `tgPhase = TG_PHASE_DEL_OLD` em `state.bin` com `tgPending` verdadeiro e o Telegram
   inacessivel.
2. Esperado: nenhum `getUpdates()` enquanto a fase progride; apos `TELEGRAM_PHASE_STUCK_MS`
   (120 s) o Serial registra `TG: fase del_old sem progresso ha ...ms; comandos liberados` e o
   `DIAG` mostra `tgStuck=1`.
3. Nesse estado `/reboot` responde e reinicia.
4. Restaurando a rede antes dos 120 s, a sequencia termina normalmente e `tgStuck` volta a 0.

### Teste S4 - Orcamento de tentativas

1. Mantenha o Telegram inacessivel e provoque uma FALTA.
2. Esperado: as tentativas sao espacadas (3, 6, 12, 24, 48, 60 s...) e, apos
   `TELEGRAM_MAX_STEP_ATTEMPTS` (20) falhas, o Serial registra
   `TG: sequencia ABORTADA motivo=tentativas esgotadas ...` com `tgPending=0` e `tgPhase=none`.
3. Ao voltar a rede, o RETORNO seguinte e enviado normalmente (nada fica preso).

### Teste S5 - Erro fatal (token invalido ou chat errado)

1. Use temporariamente um `TELEGRAM_BOT_TOKEN` invalido (ou um `TELEGRAM_CHAT_ID` de um chat do
   qual o bot nao participa).
2. Esperado: `api_ok=0 error_code=401` (ou `400 ... chat not found`) seguido de
   `sequencia ABORTADA motivo=erro fatal da API` e, no boot,
   `aviso de boot abandonado neste boot (error_code=...)`.
3. Sem rajada de requisicoes: no maximo uma tentativa por transicao, nenhuma em laco.
4. Restauradas as credenciais, a proxima transicao volta a notificar.

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
