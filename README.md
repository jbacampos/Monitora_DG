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

Os quatro sinais formam **uma** medição composta (`InputGroup`): nenhum snapshot é confirmado
enquanto qualquer entrada ainda estiver no debounce. Confirmar por canal misturaria o valor novo
de um canal com o valor ainda antigo dos outros e produziria combinações que nunca existiram (por
exemplo "rede disponível + alimentado pela rede + alimentado pelo offgrid" no meio de uma
transferência do ATS). Uma sobreposição que dure mais que o debounce é uma medição real: é
publicada e o Serial registra `AVISO: estado eletrico contraditorio ...`.

## Estrutura

```text
include/
  config.h            pinos, inversao, timings, NTP/TZ, orcamento do log, chaves TB
  types.h             PowerState, TsKind/Timestamp, AliveRecord, PersistedState, PendingRecord
  input_monitor.h     aquisicao + debounce das 4 entradas
  input_group.h       portao composto: commit so com as 4 entradas assentadas
  time_source.h       millis() + NTP; conversao SessionMillis -> Epoch
  state_store.h       state.bin (temp -> rename)
  tb_queue.h          pending.bin (log append-only para ThingsBoard)
  telemetry.h         MQTT/TLS, fila historica, snapshot de boot, heartbeat
  telegram.h          API HTTP de baixo nivel do Telegram
  telegram_notifier.h coalescimento, stickers e textos
  tg_maint.h          decisao de envio da fila visual (pura, testavel no host)
  sticker_maint.h     politica da manutencao dos stickers FALTA/RETORNO (pura, testavel no host)
  wifi_manager.h      conectividade
  ota.h               /ota
  secrets.h.example   credenciais (copie para secrets.h)
src/
  (mesmos nomes .cpp) + main.cpp
test/
  host/test_logic.cpp testes de logica pura no PC (ver TESTES.md)
```

## Persistência

- `/state.bin` = um unico `PersistedState`, escrito em `/state.tmp` e depois `rename`
  (o rename do LittleFS substitui o destino de forma atomica). Sem CRC e sem slots A/B.
  As versoes 1.6.x acrescentaram campos **no fim** da struct (fila visual em 1.6.0; manutencao dos
  stickers FALTA/RETORNO em 1.6.1); todos os campos anteriores mantem offset e tipo, entao um
  `state.bin` antigo e um **prefixo** exato do atual. Tamanhos reconhecidos: 60/68/76
  (desenvolvimento), 84 (1.3.1..1.5.0, efetivamente instalado) e 120 (firmware anterior, fila
  visual); o atual e 636. `StateStore::load()` adota o prefixo (os campos novos ficam nos valores
  seguros = zero: a fila visual comeca vazia e nenhuma exclusao/edicao e agendada para eventos
  anteriores a atualizacao), grava a migracao de forma **atomica** (temp + rename; o original e
  preservado ate o rename) e recusa qualquer tamanho **desconhecido** com diagnostico explicito
  (156, um formato abandonado que nunca foi instalado, nao e reconhecido).
- `/pending.bin` = log **append-only** de `PendingRecord` (16 B: `Timestamp` 8 + `PowerState`
  4 + `rebootReason` 1 + 3 de alinhamento), precedido por um cabecalho de formato de 4 B
  (`PENDING_MAGIC`). O `tbLogHead` (em `state.bin`) aponta o proximo registro a publicar. O
  arquivo so e truncado quando a fila drena. Um `pending.bin` no formato antigo (12 B, sem
  cabecalho) e migrado no boot para o formato atual, preservando todos os registros (com
  `rebootReason` = NONE).
- Um `.tmp` orfao nunca impede a operacao.

## Timestamps

`Timestamp` carrega uma etiqueta `TsKind`:

- `Epoch` - Unix UTC em segundos (NTP sincronizado) + fracao de milissegundo em
  `Timestamp.subsecond`. O instante publicado usa os dois campos: duas transicoes dentro do mesmo
  segundo mantem instantes distintos e nao colapsam no ThingsBoard.
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

A queda de `redeDisponivel` abre uma janela de graca (`REDE_FAIL_GRACE_MS`, 15 s): uma oscilacao
mais curta nao gera FALTA. A janela e persistida e sobrevive a um reboot; a FALTA confirmada
carrega o **instante da queda** e o estado eletrico medido naquele instante (antes de o ATS
transferir a carga), nunca o estado do fim da janela.

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
- **Exclusao impossivel**: a Bot API recusa apagar uma mensagem com mais de **48 h** e, para um
  bot que nao seja administrador com `can_delete_messages`, qualquer mensagem que ele nao tenha
  enviado (`400 message can't be deleted` / `403`). Isso e **permanente**: o id e descartado,
  registrado no Serial (`deleteMessage impossivel ...` / `... descartado sem apagar ...`) e a
  sequencia continua. O sticker antigo permanece no grupo; promover o bot a administrador
  restabelece a substituicao normal.
- **Politica de repeticao**: uma etapa que falha e repetida com backoff (3, 6, 12, 24, 48, 60 s),
  respeitando o `retry_after` de um `429`; apos `TELEGRAM_MAX_STEP_ATTEMPTS` falhas da mesma
  etapa a sequencia e abandonada e o estado volta a `tgPhase = NONE`. Erros sem recuperacao
  (401 token, 403 sem direito, 400 `chat not found`, sticker invalido, 409 webhook) abandonam
  imediatamente, sem laco de requisicoes.
- As listas de adjetivos/verbos do rascunho foram preservadas sem alteracao.
- **Estado visual (4 LEDs)**: existe sempre no maximo **uma** mensagem. Cada transicao eletrica
  **confirmada** e enfileirada (`PersistedState::visualQueue`) com o estado capturado **no instante
  da transicao** e enviada em ordem cronologica assim que a sequencia FALTA/RETORNO termina
  (`tgPhase == NONE`); uma FALTA seguida de RETORNO preserva **as duas** fotografias (a sequencia
  adia os envios, nunca os descarta). Uma queda ainda dentro da janela de graca nao entra na fila
  (so a FALTA confirmada). Ao enviar a nova linha, o novo `message_id` e o anterior (guardado como
  `visualDeletePendingId`) sao persistidos ANTES de apagar o anterior. Se a exclusao falhar de
  forma transitoria, o id fica pendente e nenhuma nova linha e enviada ate a exclusao ser
  confirmada; se a API recusar para sempre, o id e descartado e a linha atual e enviada. O texto de
  boot e a linha de boot permanecem inalterados. **Todas** as mensagens da linha visual (boot e
  transicoes) sao enviadas com `disable_notification=true` (nunca tocam o som) e a manutencao
  preventiva NUNCA apaga nem reenvia a linha visual.
- **Manutencao dos stickers FALTA/RETORNO**: cada sticker e controlado individualmente. A contagem
  comeca no horario real de envio confirmado pela API (o `date` do reply, nao o inicio da tentativa).
  As 36 h iniciam as tentativas de exclusao (a cada 15 min, respeitando `retry_after`) ate 48 h; se
  nao for possivel dentro da janela, a ocorrencia e registrada. Apos a confirmacao da exclusao, a
  mensagem textual original e **simplificada** com `editMessageText` (sem enviar nova mensagem,
  mantendo o `message_id`), preservando o instante original do evento. Exclusao e edicao sao
  operacoes independentes e persistentes: se a edicao falhar, a pendencia e retomada (inclusive
  apos reboot). Quando o `DEL_OLD` apaga os stickers do ciclo anterior antes das 36 h, a
  simplificacao correspondente entra numa fila persistida para nao perder a associacao. Os textos
  nunca sao apagados.
- **Mensagem de reinicio**: existe sempre no maximo **uma**. Mesmo esquema (Model B) da linha
  visual: o novo `message_id` e o anterior (`bootDeletePendingId`) sao persistidos ANTES de
  apagar o anterior; enquanto uma exclusao estiver pendente nenhuma nova mensagem de reinicio e
  criada. A exclusao so conta como confirmada quando a API responde `{"ok":true,"result":true}`
  (nao basta um HTTP 200: `{"ok":false,...}` e falha). O conteudo da mensagem de boot permanece
  inalterado (o motivo aparece em portugues). Independente de FALTA/RETORNO e da linha visual.
- Comandos: `/reboot`, `/ota` e `/s` (autorizados por `TELEGRAM_CHAT_ID`). `/s` responde, com envio
  silencioso (`disable_notification=true`), uma mensagem com uma linha em branco inicial, a versao do
  firmware (`FIRMWARE_VERSION`), o uptime desde o boot em dias/horas/min/seg (derivado de `millis()`,
  sem NTP, sem zeros a esquerda) e uma linha `Energia:` com os quatro indicadores na ordem rede,
  alimentacao pela rede, off-grid e gerador (a mesma figura da linha visual, `InputGroup::state()`).
  E somente leitura: nao altera o estado, nao reinicia, nao inicia OTA e nao depende de
  GitHub/ThingsBoard/NTP. O `getUpdates()` espera a sequencia de notificacao terminar, mas **nunca**
  fica preso: uma fase sem progresso por `TELEGRAM_PHASE_STUCK_MS` (120 s) libera o poll
  (`DIAG ... tgStuck=1`) e os comandos voltam a responder.

## Secrets

Copie `include/secrets.h.example` para `include/secrets.h` e preencha. `secrets.h` esta no
`.gitignore`.