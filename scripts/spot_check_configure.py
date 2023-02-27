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
    print(f"Spot name: {current_config['spot_name']}")
    print(f"Timezone: {current_config['tz_display_name']}")
    print()

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

    # TODO :: include 'manual' option to manually set time)
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

    body = {
        "spot_name": spot_name,
        "spot_lat": spot_lat,
        "spot_lon": spot_lon,
        "spot_uid": spot_uid,
        "tz_str": tz_str,
        "tz_display_name": tz_display_name,
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
    # TODO :: where to put offline mode option?


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

