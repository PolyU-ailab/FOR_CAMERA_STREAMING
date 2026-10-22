import paho.mqtt.client as mqtt

MQTT_SERVER = "43.198.136.97"
MQTT_PORT = 1883
MQTT_TOPIC = "POLYU/INTERFERENCE/set_1/cam"

# MQTT Credentials
MQTT_USERNAME = "labxra_server"
MQTT_PASSWORD = "ty0YvZ5PLPXwXj3nwA18"

# Must be different from the ESP32 client ID.
MQTT_CLIENT_ID = "POLYU/INTERFERENCE/set_1/cam/PYTHON"

client = mqtt.Client(
    callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
    client_id=MQTT_CLIENT_ID
)

# Set username and password
client.username_pw_set(
    username=MQTT_USERNAME,
    password=MQTT_PASSWORD
)

client.connect(MQTT_SERVER, MQTT_PORT, 60)
client.loop_start()

command = input(
    "Enter ON, OFF, or FRAMERATE|x: "
).strip().upper()

if command == "ON":
    message = "STREAM|1"

elif command == "OFF":
    message = "STREAM|0"

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
