flowchart TD
    A[Start Digest Auth] --> B[Receive 401 Unauthorized]
    B --> C[Extract realm, nonce, qop]
    C --> D[Generate cnonce]
    D --> E[Set nc = 00000001]
    E --> F[Compute HA1 = MD5(username:realm:password)]
    F --> G[Compute HA2 = MD5(method:uri)]
    G --> H[Compute response = MD5(HA1:nonce:nc:cnonce:qop:HA2)]
    H --> I[Build Authorization Header]
    I --> J[Send Authenticated Request]
    J --> K{200 OK?}
    K -->|Yes| L[Authentication Successful]
    K -->|No| M[Retry or Fail]
    L --> N[End]
    M --> N
