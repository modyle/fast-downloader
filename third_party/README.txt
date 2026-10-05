third_party/ - сторонний код, собранный вместе с проектом.

bearssl/
    BearSSL (реализация TLS 1.0-1.2 на чистом C, MIT, Thomas Pornin).
    Источник: зеркало https://github.com/unkaktus/bearssl
    (апстрим: https://www.bearssl.org/git/BearSSL).
    Завендорено: inc/ + src/ + LICENSE.txt
    (без tools/, samples/, test/, T0/).
    Компилируется напрямую: все src/*/*.c,
    флаги -Ithird_party/bearssl/inc -Ithird_party/bearssl/src.
    Используем: TLS-клиент (br_ssl_client_init_full),
    br_sslio_* для блочного I/O, br_x509_minimal для проверки
    сертификатов. PRNG: HMAC-DRBG внутри движка, entropy
    инжектим из ОС (RtlGenRandom / /dev/urandom).

cacert.pem
    Бандл корневых CA Mozilla (через проект certifi, данные MPL-2.0).
    Fallback-якоря, когда системное хранилище протухло (например WinXP).
    Всегда дополняется системным хранилищем ОС (важно для
    корпоративных MITM-прокси).
