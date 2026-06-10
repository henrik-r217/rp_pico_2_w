


4. Bygg
Exempel:

￼
￼
￼
Shell
mkdir build
cd build

cmake -DPICO_BOARD=pico2_w \
      -DWIFI_SSID="DittWifi" \
      -DWIFI_PASSWORD="DittLosenord" \
      ..

make -j4
Om du kör vanlig Pico W i stället:

￼
￼
￼
Shell
cmake -DPICO_BOARD=pico_w \
      -DWIFI_SSID="DittWifi" \
      -DWIFI_PASSWORD="DittLosenord" \
      ..
5. Servern måste ta emot POST
På den andra enheten ska API:t t.ex. ha denna endpoint:

￼
￼
￼
Python
@app.route("/measurements", methods=["POST"])
def receive_measurement():
    data = request.get_json()
    print(data)
    return {"status": "ok"}
Pico skickar ungefär detta:

￼
￼
￼
JSON
{
  "sensor_id": "pico-01",
  "temperature": 22.5,
  "humidity": 45.0,
  "voltage": 3.28
}
Viktigt att tänka på
Använd IP-adress först, t.ex. 192.168.1.50, inte datornamn.
Använd inte localhost från Pico — det betyder Pico själv.
Kontrollera att serverns brandvägg tillåter port 5000.
Detta exempel gör en POST en gång och avslutar.
För kontinuerlig loggning kan du lägga start_http_post() i en loop med sleep_ms(10000).
Vill du kan jag även göra en version som skickar data periodiskt var 10:e sekund, med automatisk återanslutning till Wi‑Fi och felhantering för databas/API nere.
