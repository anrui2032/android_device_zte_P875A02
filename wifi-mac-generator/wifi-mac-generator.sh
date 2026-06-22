#!/vendor/bin/sh

# data format:
# ASCII text file containing the mac
WIFI_MAC_ADDR_PATH="/mnt/vendor/persist/wifimac.dat"

# data format:
# Intf0MacAddress=00AA00BB00CC
# Intf1MacAddress=00AA00BB00CD
# END
WLAN_MAC_LOGDUMP_PATH="/logdump/wlan_mac.bin"

if [ ! -s "${WIFI_MAC_ADDR_PATH}" ]; then
    exit
fi

# Read the mac from persist
raw_mac=$(cat "$WIFI_MAC_ADDR_PATH" | sed 's/^wifiaddr://; s/0x//g; s/ //g')

# Convert to decimal
dec_mac=$(printf "%d" "0x$raw_mac")

# The MAC of the first interface is the decimal mac,
# converted to uppercase
first_mac=$(printf "%012X" "$dec_mac")

# Increment the decimal mac by one
dec_mac=$(expr $dec_mac + 1 )

# The MAC of the first interface is the decimal mac
# plus one, converted to uppercase
second_mac=$(printf "%012X" "$dec_mac")

# Write the MACs
echo "Intf0MacAddress=${first_mac}" > "${WLAN_MAC_LOGDUMP_PATH}"
echo "Intf1MacAddress=${second_mac}" >> "${WLAN_MAC_LOGDUMP_PATH}"
echo "END" >> "${WLAN_MAC_LOGDUMP_PATH}"
