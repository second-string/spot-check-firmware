import os
import sys
import asyncio
import requests
import sqlite3
import urllib.parse

from time import sleep

# Manually bundle everything the scripts need from esp-idf into the correct components/tools paths stemming from here
os.environ['IDF_PATH'] = "."
sys.path.insert(0, './components/protocomm/python')
sys.path.insert(1, './tools/esp_prov')

from tools.esp_prov.esp_prov import main as prov_main

banner = """
   _____             _      _____ _               _
  / ____|           | |    / ____| |             | |
 | (___  _ __   ___ | |_  | |    | |__   ___  ___| | __
  \___ \| '_ \ / _ \| __| | |    | '_ \ / _ \/ __| |/ /
  ____) | |_) | (_) | |_  | |____| | | |  __/ (__|   <
 |_____/| .__/ \___/ \__|  \_____|_| |_|\___|\___|_|\_\\
        | |
        |_|
"""

def configure_weather_mode(current_config):
    spot_name = ""
    spot_uid = ""
    spot_lat = ""
    spot_lon = ""
    while spot_name == "":
        spot_search_str = input(f"Enter a new surfline location (press enter keep as '{current_config['spot_name']}'): ")
        if not spot_search_str or spot_search_str == "":
            spot_name = current_config["spot_name"]
            spot_uid = current_config["spot_uid"]
            spot_lat = current_config["spot_lat"]
            spot_lon = current_config["spot_lon"]
            break;

        encoded_spot_search_str = urllib.parse.quote_plus(spot_search_str)

        surfline_res = requests.get(f"https://services.surfline.com/search/site?q={encoded_spot_search_str}&querySize=10&suggestionSize=10&newsSearch=false", headers={"Content-type": "application/json"}, timeout=4.0)

        surfline_res_dict = surfline_res.json()
        for spot in surfline_res_dict[0]["hits"]["hits"]:
            y_n = input(f"Is {spot['_source']['name']} the correct spot? (y/n): ")
            if y_n == "y":
                spot_name = current_config["spot_name"]
                spot_uid = current_config["spot_uid"]
                spot_lat = current_config["spot_lat"]
                spot_lon = current_config["spot_lon"]
                break
            elif y_n == "n":
                continue
            else:
                print("Please enter a 'y' or 'n' and press enter")
        if spot_name == "":
            print()

    return {
        "spot_name": spot_name,
        "spot_lat": spot_lat,
        "spot_lon": spot_lon,
        "spot_uid": spot_uid,
    }


def configure_custom_mode(current_config):
    custom_screen_url = ""
    custom_update_interval_secs = 0

    temp_custom_screen_url = input(f"Enter a URL to an API endpoint that returns a custom image for the screen (press enter keep as '{current_config['custom_screen_url']}'): ")
    if not temp_custom_screen_url or temp_custom_screen_url == "":
        custom_screen_url = current_config["custom_screen_url"]
    else:
        custom_screen_url = temp_custom_screen_url


    while 1:
        print()
        temp_update_interval_secs = input(f"Enter the length of time the device should wait in between requests to the API endpoint in seconds, minimum 900 seconds / 15 minutes (press enter to keep as '{current_config['custom_update_interval_secs']}'): ")
        if not temp_update_interval_secs or temp_update_interval_secs == "":
            custom_update_interval_secs = current_config["custom_update_interval_secs"]
        elif int(temp_update_interval_secs) < 900:
            print("Value too low, minimum is 900 seconds (15 minutes)")
        else:
            custom_update_interval_secs = int(temp_update_interval_secs)
            break

    return {
        "custom_screen_url": custom_screen_url,
        "custom_update_interval_secs": custom_update_interval_secs,
    }

def configure():
    db_conn = sqlite3.connect("timezones.db")
    cursor = db_conn.cursor()

    print("Searching for Spot Check device on the network...")
    success = False
    while not success:
        try:
            res = requests.get("http://spot-check.local./health", headers={"Content-type": "application/json"}, timeout=0.2)
            res.raise_for_status()
            success = True
        except Exception as e:
            success = False
            print("Could not find Spot Check device on the network. Make sure you you have connected the device to the network using step 1 in the main menu, and that this computer is connected to the same network.")
            print("Retrying in 5 seconds...")
            sleep(5)

    print("Found device!")
    print()
    print("Fetching device current configuration...")
    try:
        res = requests.get("http://spot-check.local./current_configuration", headers={"Content-type": "application/json"}, timeout=0.2)
        res.raise_for_status()
    except Exception as e:
        raise e

    current_config = res.json()

    print()
    print("Current configuration stored on device:")
    print(f"Timezone: {current_config['tz_display_name']}")
    print(f"Device mode: {current_config['operating_mode']}")
    if current_config['operating_mode'] == "weather":
        print(f"Spot name: {current_config['spot_name']}")
    elif current_config['operating_mode'] == "custom":
        print(f"Custom external URL: {current_config['custom_screen_url']}")
        print(f"Screen update interval (seconds): {int(current_config['custom_update_interval_secs'])}")
    else:
        assert 0
    print()

    # TODO :: include 'manual' option to manually set time)
    # TODO :: offline mode?
    tz_str = ""
    tz_display_name = ""
    while tz_str == "":
        tz_search_str = input(f"Enter a major city to set the timezone of the device, i.e. 'Los Angeles' or 'Berlin' (press enter to keep as '{current_config['tz_display_name']}'): ")
        if not tz_search_str or tz_search_str == "":
            tz_str = current_config["tz_str"]
            tz_display_name = current_config["tz_display_name"]
            break

        tz_search_str = tz_search_str.replace(" ", "_")

        for row in cursor.execute("SELECT * FROM timezones WHERE display_name LIKE ?", ["%" + tz_search_str + "%"]):
            y_n = input(f"Is {row[0]} the correct timezone? (y/n): ")
            if y_n == "y":
                tz_str = row[1]
                tz_display_name = row[0]
                break
            elif y_n == "n":
                continue
            else:
                print("Please enter a 'y' or 'n' and press enter")
        if tz_str == "":
            print()

    choice = None
    mode = ""
    print()
    print("Select device mode:")
    while choice != "1" and choice != "2":
        print("1) Weather mode: display the time, current weather conditions, and two weather charts on the screen")
        print("2) Custom mode: display custom data on the screen hosted on your own external API")
        choice = input("(1/2): ")
        print()

    extra_vals = {}
    if choice == "1":
        mode = "weather"
        extra_vals = configure_weather_mode(current_config)
    elif choice == "2":
        mode = "custom"
        extra_vals = configure_custom_mode(current_config)

    print(extra_vals)
    body = {
        "operating_mode": mode,
        "tz_str": tz_str,
        "tz_display_name": tz_display_name,
        # TODO :: how to merge extra_vals dict with this one
    }

    print()
    print("Saving new configuration to device...")
    try:
        res = requests.post("http://spot-check.local./configure", data=body, headers={"Content-type": "application/json"}, timeout=0.2)
        res.raise_for_status()
    except Exception as e:
        raise e

    print("Success!")

    cursor.close()
    db_conn.close()


def provision():
    print("Please plug in your Spot Check device to power")
    print("When the 'Spot Check configuration' wifi network appears, please connect this computer to it.")
    print()
    input("Press enter when connected to 'Spot Check configuration'\n")

    ssid = input("Enter the SSID of the wifi network to connect the device to: ")
    pw = input(f"Enter the password to the '{ssid}' network (if there is no password, just press enter): ")
    print()

    # ssid = "PYUR 19ACA"
    # pw = "NK77H76Gprxe"

    sys.argv[1:] = [
        "--transport", "softap",
        "--ssid", ssid,
        "--passphrase", pw,
    ]

    asyncio.run(prov_main())

def clear_nvs():
    print("Searching for Spot Check device on the network...")
    success = False
    while not success:
        try:
            res = requests.get("http://spot-check.local./health", headers={"Content-type": "application/json"}, timeout=0.2)
            res.raise_for_status()
            success = True
        except Exception as e:
            success = False
            print("Could not find Spot Check device on the network. Make sure you you have connected the device to the network using step 1 in the main menu, and that this computer is connected to the same network.")
            print("Retrying in 5 seconds...")
            sleep(5)

    print("Found device!")
    print()
    print("Resetting device to factory defaults...")
    try:
        params = {
            "key": "sekrit", # :)
        }
        res = requests.post("http://spot-check.local./clear_nvs", params=params, headers={"Content-type": "application/json"}, timeout=0.2)
        res.raise_for_status()
    except Exception as e:
        raise e
    print("Success!")




if __name__ == "__main__":
    print(banner)
    print("Welcome to device configuration. Choose an option.")

    choice = None
    while choice != "1" and choice != "2" and choice != "3":
        print("1) Set up Spot Check device wifi connection")
        print("2) Configure Spot Check device settings")
        print("3) Reset device to factory defaults")
        choice = input("(1/2/3): ")
        print()

    if choice == "1":
        provision()
    elif choice == "2":
        configure()
    elif choice == "3":
        clear_nvs()

