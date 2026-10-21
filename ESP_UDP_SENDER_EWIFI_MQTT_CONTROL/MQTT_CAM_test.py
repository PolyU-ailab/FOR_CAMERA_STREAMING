import paho.mqtt.client as mqtt

MQTT_SERVER = "43.198.136.97"
MQTT_PORT = 1883
MQTT_TOPIC = "POLYU/CAMCONTROLLER/CAM50"

# Must be different from the ESP32 client ID.
MQTT_CLIENT_ID = "POLYU/CAMCONTROLLER/CAM50/PYTHON"

client = mqtt.Client(
    callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
    client_id=MQTT_CLIENT_ID
)

client.connect(MQTT_SERVER, MQTT_PORT, 60)
client.loop_start()

command = input(
    "Enter ON, OFF, or FRAMERATE|x: "
).strip().upper()

if command == "ON":
    message = "CAM|1"

elif command == "OFF":
    message = "CAM|0"

elif command.startswith("FRAMERATE|"):
    value = command.split("|", 1)[1]

    if not value.isdigit():
        print("Invalid frame interval.")
        client.disconnect()
        client.loop_stop()
        exit()

    message = f"FRAMERATE|{value}"

else:
    print("Invalid command.")
    client.disconnect()
    client.loop_stop()
    exit()

result = client.publish(
    MQTT_TOPIC,
    message,
    qos=1
)

result.wait_for_publish()

print(f"Published: {message}")

client.disconnect()
client.loop_stop()
