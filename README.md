# Monitora_DG

Firmware ESP8266 que monitora quatro sinais digitais da alimentação elétrica do sítio e
publica a telemetria no ThingsBoard. O Telegram é usado **somente** para eventos de
`redeDisponivel`.

Não existe dependência de ER605, Vivo, Claro, Deco, Cloudflare ou de qualquer outro
equipamento/serviço específico. Não há módulo de temperatura nem Adafruit IO.

## Entradas

| Sinal | NodeMCU |
|---|---|
| redeDisponivel | D1 / GPIO5 |
| alimentacaoRede | D2 / GPIO4 |
| alimentacaoOffgrid | D5 / GPIO14 |
| alimentacaoGerador | D6 / GPIO12 |

Todas são tratadas **logicamente** como ACTIVE-HIGH. A inversão física é configurada por
canal em `include/config.h` (`INPUT_*_INVERTED`); o restante do firmware só vê o nível
normalizado. As quatro entradas têm debounce; só uma transição confirmada gera evento.

## Estrutura

```text
include/
  config.h            pinos, inversao, timings, NTP/TZ, orcamento do log, chaves TB
  types.h             PowerState, TsKind/Timestamp, AliveRecord, PersistedState, PendingRecord
  input_monitor.h     aquisicao + debounce das 4 entradas
  time_source.h       millis() + NTP; conversao SessionMillis -> Epoch
  state_store.h       state.bin (temp -> rename)
  tb_queue.h          pending.bin (log append-only para ThingsBoard)
  telemetry.h         MQTT/TLS, fila historica, snapshot de boot, heartbeat
  telegram.h          API HTTP de baixo nivel do Telegram
  telegram_notifier.h coalescimento, stickers e textos
  wifi_manager.h      conectividade
  ota.h               /ota
  secrets.h.example   credenciais (copie para secrets.h)
src/
  (mesmos nomes .cpp) + main.cpp
```

## Persistência

- `/state.bin` = um unico `PersistedState`, escrito em `/state.tmp` e depois `rename`
  (o rename do LittleFS substitui o destino de forma atomica). Sem CRC e sem slots A/B.
- `/pending.bin` = log **append-only** de `PendingRecord` (16 B: `Timestamp` 8 + `PowerState`
  4 + `rebootReason` 1 + 3 de alinhamento), precedido por um cabecalho de formato de 4 B
  (`PENDING_MAGIC`). O `tbLogHead` (em `state.bin`) aponta o proximo registro a publicar. O
  arquivo so e truncado quando a fila drena. Um `pending.bin` no formato antigo (12 B, sem
  cabecalho) e migrado no boot para o formato atual, preservando todos os registros (com
  `rebootReason` = NONE).
- Um `.tmp` orfao nunca impede a operacao.

## Timestamps

`Timestamp` carrega uma etiqueta `TsKind`:

- `Epoch` - Unix UTC em segundos (NTP sincronizado).
- `SessionMillis` - `millis()` da sessao atual (evento antes do NTP); convertido para
  `Epoch` assim que o NTP sincroniza.
- `LostSession` - `SessionMillis` que sobreviveu a um reboot; o tempo absoluto e
  **desconhecido** (nunca fabricamos um epoch).

NTP: `pool.ntp.org`; timezone de apresentacao: America/Sao_Paulo (`<-03>3`). Os valores
internos sao sempre UTC.

## Boot / reboot

Ordem: LittleFS -> repara `.tmp`/cauda parcial -> carrega estado -> GPIO + debounce ->
reconcilia -> NTP -> monitora GPIO. Depois de estabilizar as entradas, compara o
`redeDisponivel` persistido com o fisico:

| Antes | Agora | Acao |
|---|---|---|
| SIM | SIM | nada |
| SIM | NAO | FALTA reconstruida com o ts do ultimo alive local com rede=SIM |
| NAO | SIM | RETORNO reconstruido com o ts atual (forcado no Telegram, regra do reboot) |
| NAO | NAO | nada |

Primeira inicializacao: nenhum evento artificial. Em toda inicializacao e enfileirado
um snapshot obrigatorio dos quatro estados para o ThingsBoard.

## ThingsBoard

- `v1/devices/me/telemetry`: cada transicao confirmada gera um snapshot dos 4 estados com
  o timestamp original; eventos pendentes sao enviados em ordem cronologica.
- **Historico de reinicios**: em todo boot o snapshot obrigatorio carrega adicionalmente a
  chave `reboot_reason` com o **codigo NUMERICO** do motivo (tabela abaixo); as transicoes
  normais nunca a possuem. No Telegram/Serial o mesmo codigo vira a descricao em portugues
  (tabela unica `rebootReasonDescription()`). Como usa a mesma fila `pending.bin`, a publicacao
  e at-least-once, funciona sem Wi-Fi, mantem o timestamp original do boot e respeita a ordem
  cronologica.

  | codigo | motivo (`ESP.getResetReason()` / OTA) | descricao (pt-BR) |
  |---|---|---|
  | 1 | `Power On` | Energização (power-on) |
  | 2 | `Hardware Watchdog` | Watchdog de hardware |
  | 3 | `Exception` | Exceção |
  | 4 | `Software Watchdog` | Watchdog de software |
  | 5 | `Software/System restart` | Reinício por software |
  | 6 | `Deep-Sleep Wake` | Despertar de deep sleep |
  | 7 | `External System` | Reset externo |
  | 8 | `Unknown` | Desconhecido |
  | 9 | OTA (`pendingBootReason`) | Atualização (OTA) |
- `v1/devices/me/attributes`: heartbeat de supervisao (~60 s) com `last_alive`,
  `uptime_s`, `wifi_rssi`. O `last_alive` do TB **nao** e usado para reconstrucao.
- Fila persistente append-only (orcamento ~512 KB => dezenas de milhares de eventos).
  Nunca descarta silenciosamente; se a Flash esgotar, conta em `tbGapCount` e continua
  monitorando. At-least-once e aceito (o `tbLogHead` e persistido de tempos em tempos).

## Telegram

- Somente `redeDisponivel` gera notificacao. As outras tres entradas nunca.
- **Coalescimento**: a pendencia e um unico alvo (`tgNotifiedRede` vs estado atual).
  Varias FALTAS/RETORNOS durante uma indisponibilidade resultam em **uma** notificacao
  coerente ao voltar.
- **Stickers**: FALTA apaga os **dois** stickers do ciclo anterior e envia o novo FALTA;
  RETORNO envia o Huno sem apagar o FALTA. Depois de um RETORNO ha exatamente dois
  stickers; a proxima FALTA apaga ambos. Textos nunca sao apagados.
- Fases persistidas (DEL_OLD/SEND_FALL/TEXT_FALL/SEND_RET/TEXT_RET) permitem retomar apos
  reboot. Os indices de adjetivo/verbo sao escolhidos uma vez por ciclo.
- As listas de adjetivos/verbos do rascunho foram preservadas sem alteracao.
- **Estado visual (4 LEDs)**: existe sempre no maximo **uma** mensagem. Ao enviar a nova
  linha, o novo `message_id` e o anterior (guardado como `visualDeletePendingId`) sao
  persistidos ANTES de apagar o anterior. Se a exclusao falhar, o id fica pendente e nenhuma
  nova linha e enviada ate a exclusao ser confirmada (as mudancas continuam sendo coalescidas
  no estado atual); so apos resolver a pendencia a linha atual e enviada. O texto de boot e a
  linha de boot permanecem inalterados.
- **Mensagem de reinicio**: existe sempre no maximo **uma**. Mesmo esquema (Model B) da linha
  visual: o novo `message_id` e o anterior (`bootDeletePendingId`) sao persistidos ANTES de
  apagar o anterior; enquanto uma exclusao estiver pendente nenhuma nova mensagem de reinicio e
  criada. A exclusao so conta como confirmada quando a API responde `{"ok":true,"result":true}`
  (nao basta um HTTP 200: `{"ok":false,...}` e falha). O conteudo da mensagem de boot permanece
  inalterado (o motivo aparece em portugues). Independente de FALTA/RETORNO e da linha visual.
- Comandos: `/reboot` e `/ota` (autorizados por `TELEGRAM_CHAT_ID`).

## Secrets

Copie `include/secrets.h.example` para `include/secrets.h` e preencha. `secrets.h` esta no
`.gitignore`.