sequenceDiagram
    autonumber
    participant ESP32
    participant FB as FRITZ!Box
    participant PH as Phones (**11)

    Note over ESP32: T0 — Button Press

    ESP32->>FB: INVITE (no auth)
    Note right of FB: FRITZ!Box waits ~50–150 ms
    FB-->>ESP32: 401 Unauthorized

    ESP32->>ESP32: Compute Digest (INVITE)

    ESP32->>FB: INVITE (with auth)
    Note right of FB: FRITZ!Box responds immediately
    FB-->>ESP32: 100 Trying

    FB-->>PH: Ring group **11
    FB-->>ESP32: 180 Ringing

    PH-->>FB: 200 OK (answered)
    FB-->>ESP32: 200 OK

    ESP32->>FB: ACK
    ESP32->>FB: Start RTP

    Note over ESP32: RTP active

    alt Remote hangs up
        PH-->>FB: BYE
        FB-->>ESP32: BYE
        ESP32->>FB: 200 OK
    else Local hangs up
        ESP32->>FB: BYE
        FB-->>ESP32: 200 OK
    end

    ESP32->>ESP32: Stop RTP
