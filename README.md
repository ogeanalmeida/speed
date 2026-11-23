# Bike Speed (PWA)

App de velocímetro de bike com ESP32, feito a partir do Figma "Bike Speed UI v1.0".
Tudo fica no aparelho (localStorage). Nada vai para a nuvem.

## Conexão por cabo USB (protótipo, computador)

O app lê um Arduino ligado por cabo ao computador usando **Web Serial** (Chrome ou Edge no Mac/PC;
precisa de https, como no GitHub Pages, ou localhost). Não funciona no Safari nem no iPhone.

1. Placa: **ESP32** com `firmware/bike_speed_esp32/bike_speed_esp32.ino` (v1.5, reed no GPIO27) ou, para teste, **Arduino Uno/Nano** com `firmware/bike_speed_usb/bike_speed_usb.ino` (reed no pino 2). As duas falam o mesmo protocolo.
2. Feche o Monitor Serial do Arduino IDE (ele ocupa a porta).
3. No app: engrenagem > **Conectar por cabo USB (computador)** > escolha a porta (`usbmodem…`).
4. Nas próximas vezes o app reconecta sozinho se a porta já foi autorizada.

Protocolo (texto, 115200 baud): `T,vel*10,media*10,max*10,metros,bateria,flags` (Arduino → app, 5×/s)
e `C,1,b0,b1,b2,b3` / `C,2` / `C,3,b0,b1` (app → Arduino), os mesmos comandos do Bluetooth.
O mesmo firmware ESP32 atende Bluetooth, Wi-Fi e cabo ao mesmo tempo.

## Publicar no GitHub Pages

Envie o conteúdo desta pasta (index.html na raiz) para um repositório e ative
Settings > Pages > Deploy from branch (main, /root). O app abre em `https://SEU-USUARIO.github.io/REPO/`
e pode ser instalado na tela inicial de qualquer aparelho.

## Telas
- **Tela 1, objetivo do dia:** ajuste o objetivo (−1 / +1 km, segure para repetir), metas rápidas (10, 15, 21, 30 km), KM acumulados com botão **Zerar** e **Iniciar percurso**.
- **Tela 2, painel:** velocidade atual, média e máxima; distância percorrida com barra de progresso, restante e objetivo; KM acumulados; bateria do ESP32 com cor que muda gradualmente (vermelho até 15%, laranja em 50%, verde em 100%).
- **Finalizar percurso** soma a distância ao acumulado. O acumulado só zera quando você toca em **Zerar**.
- Toque no indicador do topo (Simulação / ESP32) para conectar, trocar de modo e ajustar a roda.

## Testar agora (modo simulação)
O app abre em modo simulação: gera velocidade e distância sozinho. Em "Conexão e ajustes" dá para acelerar o tempo (10× e 60×) e mexer na bateria para ver as cores.

## Hospedar (precisa de HTTPS)
Service worker (uso offline), instalação e Bluetooth só funcionam em HTTPS ou em `localhost`.
- Mais simples: arraste esta pasta no **Netlify Drop** (app.netlify.com/drop), ou use **Cloudflare Pages** / **GitHub Pages**.
- Local: `python3 -m http.server 8000` e abra `http://localhost:8000`.
- Instalar: Chrome no Android, menu, **Instalar app**.
- Ao trocar qualquer arquivo, aumente o número em `build.js`. Ele aparece pequeno no rodapé do app e faz o celular baixar a versão nova.

## Fonte Clash Display (igual ao Figma)
Baixe na Fontshare (gratuita) e coloque em `fonts/`:
- `ClashDisplay-Medium.woff2`
- `ClashDisplay-Regular.woff2`

Sem esses arquivos o app usa a Lexend (já incluída), parecida. O app mostra alguns erros 404 no console por causa disso, sem prejuízo.

## Tipo de bike
Na Tela 1, o botão "Aro 29" (topo) abre a seleção: 29, 27,5, 26, 24, 20, 700c ou Personalizado (digite a circunferência medida em mm). Ao confirmar, o app salva e envia a circunferência ao ESP32 (comando 0x03). Os valores são médias; medir a sua roda dá mais precisão.

## Conexão: Bluetooth ou Wi-Fi (os dois funcionam)
- **Bluetooth:** abra o app pela versão **https** (a hospedada), toque no indicador do topo e em "Conectar por Bluetooth". Só Chrome no Android/computador.
- **Wi-Fi:** com o celular na mesma rede do ESP32, o app é aberto **pelo próprio ESP32**, em `http://bikespeed.local` (ou `http://IP-DO-ESP32`). Ele conecta sozinho. Na versão https, o botão "Abrir pelo Wi-Fi do ESP32" leva a esse endereço. Navegadores bloqueiam páginas https de chamar endereços http da rede local, por isso o app é servido pelo ESP32.
- Na página servida pelo ESP32 o Bluetooth não existe (o navegador só libera Bluetooth em https) e os dados salvos (km acumulados, meta) ficam separados dos da versão https, porque cada endereço tem seu próprio armazenamento.
- Para o ESP32 entrar no seu Wi-Fi: preencha `WIFI_SSID` e `WIFI_PASS` no sketch, ou use a página de atualização (BOOT por 3 s, rede `BikeSpeed-Update`, `http://192.168.4.1`, campo "Wi-Fi de casa").
- Sempre que mudar o app, rode `python3 firmware/make_webapp.py` para atualizar a cópia que vai dentro do ESP32 (`webapp.h`) e grave o firmware de novo.

## Painel do ESP32 e atualização por Wi-Fi
Com o ESP32 no Wi-Fi da casa, abra `http://bikespeed.local` (ou o IP do Monitor Serial): é o **painel** (status, firmware, Wi-Fi). O botão "Abrir velocímetro" leva ao app em `/app`.
- **Atualizar firmware sem cabo e sem BOOT:** no painel, em Firmware // OTA, envie o `bike_speed_esp32.ino.bin` (Arduino IDE: Sketch › Exportar Binário Compilado) com a senha OTA. Também dá pelo Arduino IDE: Ferramentas › Porta › "bikespeed" (rede) › Carregar, com a mesma senha.
- **Senha OTA padrão:** `bike1234` (usuário `admin`). Troque em `OTA_PASSWORD` no sketch e grave de novo.
- **Resgate:** se esquecer a senha ou perder o Wi-Fi, segure BOOT por 3 s: o ESP32 cria a rede `BikeSpeed-Update` (senha `bikespeed1`) com uma página de envio (`http://192.168.4.1`) sem senha OTA.

## Firmware do ESP32 (versão funcional)
O sketch pronto está em `firmware/bike_speed_esp32/` (dois arquivos que precisam ficar juntos na mesma pasta: `bike_speed_esp32.ino` e `webapp.h`).
1. Arduino IDE, placa **ESP32 Dev Module** (pacote "esp32" da Espressif). Não precisa instalar bibliotecas.
2. Sensor da roda: um fio no **GND** e o outro no **GPIO 27**.
3. Grave, abra o app, toque no indicador do topo, **Conectar ESP32 (Bluetooth)** e escolha `BikeSpeed`.
4. Confira a circunferência da roda em "Conexão e ajustes". O app envia o valor ao ESP32, que guarda na memória.
5. Sem sensor, dá para testar encostando o fio do GPIO 27 no GND: cada toque é uma volta.

Bateria: por padrão o firmware envia 100% (use `USE_BATTERY_ADC 1` no sketch para ler uma bateria de lítio por divisor de tensão no GPIO 34).

## Protocolo Bluetooth (para o sketch do Arduino IDE)
O app usa Web Bluetooth. Funciona no **Chrome do Android e do computador**. No iPhone o Safari e o Chrome não têm Web Bluetooth.

- Nome do ESP32: começa com `BikeSpeed` (ex.: `BikeSpeed`)
- Serviço: `b1cec10c-0001-4e5a-9a3b-5eb1c0de0001`
- Telemetria (NOTIFY): `b1cec10c-0002-4e5a-9a3b-5eb1c0de0001`, 12 bytes, little-endian

| Bytes | Tipo | Conteúdo |
|---|---|---|
| 0–1 | uint16 | velocidade atual (km/h × 10) |
| 2–3 | uint16 | velocidade média (km/h × 10) |
| 4–5 | uint16 | velocidade máxima (km/h × 10) |
| 6–9 | uint32 | distância do percurso em metros |
| 10 | uint8 | bateria (0 a 100 %) |
| 11 | uint8 | flags (bit 0 = em movimento, bit 1 = percurso em andamento) |

- Comando (WRITE): `b1cec10c-0003-4e5a-9a3b-5eb1c0de0001`

| Primeiro byte | Dados | Ação |
|---|---|---|
| `0x01` | uint32 objetivo em metros | iniciar percurso: o ESP32 zera distância, média e máxima |
| `0x02` | nenhum | finalizar percurso |
| `0x03` | uint16 circunferência da roda em mm | calibrar a roda |

O ESP32 faz a conta da velocidade com o sensor da roda (circunferência ÷ tempo entre pulsos) e manda a telemetria cerca de 4 vezes por segundo.
