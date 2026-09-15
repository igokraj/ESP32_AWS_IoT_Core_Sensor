# ESP32 Sensor AWS Iot Core

Celem tego projektu było zintegrowanie czujnika temperatury i wilgotności na ESP32 z AWS IoT Core -> publiczną usługą firmy Amazon służącą do łączenia urządzeń IoT z chmurą, bezpiecznego zarządzania nimi oraz przetwarzania generowanych przez nie danych. AWS działa jako broker MQTT i stanowi punkt wejścia dla wszelkich danych przesyłanych przez urządzenie do chmury. Urządzenie obsługuje również aktualizacje firmware przez sieć (OTA) z wykorzystaniem AWS IoT Jobs i Amazon S3, z automatycznym powrotem do poprzedniej wersji w razie awarii.

Do użytku w przyszłości stworzyłem też krótkie podsumowanie obsługi AWS IoT w formacie PDF -> [instructions/AWS-IoT-Core-ESP32-instrukcja.pdf](instructions/AWS-IoT-Core-ESP32-instrukcja.pdf)

The goal of this project was to integrate a temperature and humidity sensor based on the ESP32 with AWS IoT Core—Amazon’s public service for connecting IoT devices to the cloud, securely managing them, and processing the data they generate. AWS acts as an MQTT broker and serves as the entry point for all data transmitted by the device to the cloud. The device also supports over-the-air (OTA) firmware updates through AWS IoT Jobs and Amazon S3, with automatic rollback if a new version fails.

For future reference, I also created a short PDF summary of working with AWS IoT -> [instructions/AWS-IoT-Core-ESP32-instruction.pdf](instructions/AWS-IoT-Core-ESP32-instruction.pdf)


## Breadboard

<img src="images/Breadboard.jpg" alt="Project mounted on a breadboard" width="400">

## AWS Dashboard

![CloudWatch dashboard](images/Dashboard.png)

## Policy (JSON) 

```json
{
  "Version": "2012-10-17",
  "Statement": [
    {
      "Effect": "Allow",
      "Action": "iot:Connect",
      "Resource": "arn:aws:iot:eu-central-1:<ACCOUNT_ID>:client/<CLIENT_ID>"
    },
    {
      "Effect": "Allow",
      "Action": "iot:Publish",
      "Resource": "arn:aws:iot:eu-central-1:<ACCOUNT_ID>:topic/sensors/<CLIENT_ID>/data"
    },
    {
      "Effect": "Allow",
      "Action": ["iot:Publish", "iot:Receive"],
      "Resource": "arn:aws:iot:eu-central-1:<ACCOUNT_ID>:topic/$aws/things/<CLIENT_ID>/jobs/*"
    },
    {
      "Effect": "Allow",
      "Action": "iot:Subscribe",
      "Resource": "arn:aws:iot:eu-central-1:<ACCOUNT_ID>:topicfilter/$aws/things/<CLIENT_ID>/jobs/*"
    }
  ]
}
```

## Rule Settings 

![Rule settings](images/RULE.png)

## How it works

1. The ESP32-S3 wakes up from deep sleep and connects to WiFi.
2. It reads temperature and humidity from an HTU21D sensor over I2C.
3. The reading is published to AWS IoT Core over MQTT as JSON, e.g. `{"t":23.45,"h":58.20}`, on the topic `sensors/<client_id>/data`.
4. The device asks AWS IoT Jobs for a pending firmware update. If a job with a new version is waiting, the image is downloaded from a private S3 bucket via a presigned URL and installed in the second OTA partition. The new version reports the job as succeeded, or the device rolls back to the previous version if it cannot reach AWS.
5. The device goes back to deep sleep for 60 seconds.

The readings are visualized on an Amazon CloudWatch dashboard.

## OTA

![S3 bucket with firmware and job document](images/Bucket.png)

![IAM role for presigned URLs](images/IAM%20Role.png)

![Completed OTA job](images/Remote%20Job.png)

## Design assumptions

- **Secure connection:** MQTT runs over TLS (port 8883) with mutual authentication. The device verifies AWS with the Amazon Root CA, and AWS verifies the device with its X.509 certificate issued by AWS IoT Core.
- **Low power:** the radio is on only for a few seconds per wake-up. If WiFi or MQTT is not reachable within the timeout, the reading is dropped and the device sleeps instead of draining the battery.
- **No secrets in the repository:** WiFi credentials, the AWS endpoint and certificates are kept in files excluded by `.gitignore`.
