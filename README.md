# Monitora_DG

Novo firmware para o ESP8266 responsável por monitorar:

- disponibilidade da rede externa da concessionária Equatorial;
- alimentação das cargas pela rede;
- alimentação das cargas pelo inversor off-grid;
- alimentação das cargas pelo gerador.

A medição de temperatura e a integração com Adafruit IO foram removidas.

## Estrutura

```text
Monitora_DG/
├── include/
│   ├── config.h
│   ├── inputs.h
│   ├── notifications.h
│   ├── ota.h
│   ├── secrets.h.example
│   ├── state_store.h
│   ├── telegram.h
│   ├── telemetry.h
│   ├── types.h
│   └── wifi_manager.h
├── src/
│   ├── inputs.cpp
│   ├── main.cpp
│   ├── notifications.cpp
│   ├── ota.cpp
│   ├── state_store.cpp
│   ├── telegram.cpp
│   ├── telemetry.cpp
│   └── wifi_manager.cpp
├── platformio.ini
└── .gitignore
```

## GPIOs

| Sinal | NodeMCU |
|---|---|
| Rede disponível | D1 / GPIO5 |
| Alimentação pela rede | D2 / GPIO4 |
| Alimentação pelo off-grid | D5 / GPIO14 |
| Alimentação pelo gerador | D6 / GPIO12 |

Os quatro conversores ópticos são considerados ativos em HIGH.

## Persistência

O estado é salvo em dois slots LittleFS:

- `/state_a.bin`
- `/state_b.bin`

Cada registro possui número de sequência e CRC32. O firmware sempre grava no slot menos recente. Assim, uma queda de energia durante uma gravação não elimina automaticamente o último estado válido.

São persistidos:

- disponibilidade da Equatorial;
- alimentação pela rede;
- alimentação pelo off-grid;
- alimentação pelo gerador;
- ID do último sticker de **queda de energia**.

## Telegram

As mensagens mantêm o formato do programa original:

```text
O <adjetivo> Sr. Oliver <verbo>:
🕯️ FALTOU LUZ! 🕯️
```

e:

```text
O <adjetivo> Sr. Huno <verbo>:
💡 A LUZ VOLTOU! 💡
```

Adjetivos e verbos percorrem ciclos embaralhados, evitando repetição até que todos os elementos daquele grupo tenham sido usados.

### Política de stickers

**Queda da Equatorial**
1. tenta apagar o sticker de queda anterior;
2. envia o sticker Oliver;
3. grava o novo ID do sticker de queda;
4. envia a mensagem textual.

**Retorno da Equatorial**
1. envia o sticker Huno;
2. NÃO apaga o sticker anterior;
3. NÃO substitui o ID persistido do sticker de queda;
4. envia a mensagem textual.

As mensagens de texto nunca são apagadas.

## ThingsBoard no Neo

A telemetria usa MQTT/TLS na porta 8883 e o tópico:

```text
v1/devices/me/telemetry
```

ThingsBoard aceita o access token do dispositivo como usuário MQTT e senha vazia. A documentação oficial confirma esse formato e o tópico de telemetria. 

Chaves enviadas:

```text
rede_disponivel
alimentacao_rede
alimentacao_offgrid
alimentacao_gerador
```

Além disso, no heartbeat:

```text
uptime_s
wifi_rssi
```

O estado é publicado imediatamente após cada conexão MQTT e nas mudanças. O heartbeat é configurado para 5 minutos.

## Telegram: comandos

O mecanismo antigo de reset via Adafruit desapareceu.

Comandos previstos:

```text
/reboot
/ota
```

A autorização é feita pelo `TELEGRAM_CHAT_ID`.

A rotina `/ota` usa `OTA_FIRMWARE_URL` de `secrets.h` e executa atualização HTTPS. O servidor deve disponibilizar o `.bin` correspondente ao firmware.

## Secrets

Copie:

```text
include/secrets.h.example
```

para:

```text
include/secrets.h
```

e preencha os dados reais.

`include/secrets.h` está no `.gitignore`.

## Cline

A primeira etapa recomendada no Cline é revisar este projeto sem alterar código e produzir:

1. mapa de dependências;
2. riscos de bloqueio do loop;
3. riscos de corrupção do estado;
4. revisão da política de stickers;
5. plano de testes de bancada;
6. proposta de implementação da OTA.

Somente depois disso deve começar a implementação incremental.
