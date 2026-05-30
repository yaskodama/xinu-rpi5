# Pi 4 TFTP netboot setup — リモート kernel 更新ワークフロー

目標: SD card swap せずに `make pi4` → Pi 4 電源 OFF/ON だけで新カーネル起動.

## 1. Mac 側: dnsmasq + TFTP (proxyDHCP)

```bash
# Install
brew install dnsmasq

# TFTP root
sudo mkdir -p /opt/homebrew/var/tftp
sudo chown $USER /opt/homebrew/var/tftp

# Config (proxyDHCP — IP は既存 router が提供、boot info だけを足す)
cat > /opt/homebrew/etc/dnsmasq.conf <<'EOF'
interface=en0                           # ← active Ethernet/WiFi interface
bind-interfaces
dhcp-range=192.168.3.200,proxy,255.255.255.0
tftp-root=/opt/homebrew/var/tftp
enable-tftp
pxe-prompt="Pi 4 netboot",1
pxe-service=0,"Raspberry Pi Boot"
log-dhcp
log-queries
EOF

# Start
sudo brew services start dnsmasq

# Verify TFTP works
echo "test" > /opt/homebrew/var/tftp/test.txt
tftp -e localhost <<< "get test.txt /tmp/x; quit" && cat /tmp/x
```

## 2. TFTP root にブートファイル一式

Pi 4 の SD card から必要ファイルをコピー:

```bash
# SD card 挿入後
cp /Volumes/bootfs/bootcode.bin   /opt/homebrew/var/tftp/   # (Pi 4 では未使用だが念のため)
cp /Volumes/bootfs/start4.elf     /opt/homebrew/var/tftp/   # Pi 4 firmware
cp /Volumes/bootfs/fixup4.dat     /opt/homebrew/var/tftp/   # Pi 4 firmware
cp /Volumes/bootfs/bcm2711-rpi-4-b.dtb  /opt/homebrew/var/tftp/
cp /Volumes/bootfs/config.txt     /opt/homebrew/var/tftp/
cp /Users/kodamay/projects/xinu-rpi4/compile/kernel8.img  /opt/homebrew/var/tftp/
```

## 3. Pi 4 EEPROM BOOT_ORDER に Network を追加

**現状**: Pi 4 デフォルト `BOOT_ORDER=0xf41` (SD → USB → loop) で Network 含まず.

**変更必要**: `0xf241` (SD → USB → Net → loop). SD が無効/欠落なら Net へ fallback.

これは現在の xinu kernel からは出来ない (vcgencmd 相当の EEPROM 設定 API なし).
**一度だけ** Raspberry Pi OS を SD で起動して `sudo rpi-eeprom-config --edit` する必要あり.

代替: SD card に `recovery.bin` + 設定 BIN を置く方式もある (rpi-eeprom-update).

## 4. SD card の kernel8.img をリネーム

```bash
# SD 挿入後
mv /Volumes/bootfs/kernel8.img /Volumes/bootfs/kernel8.local-backup
sync
diskutil eject /Volumes/bootfs
```

これで Pi 4 ブート時 SD に kernel が無く → Network fallback → TFTP fetch.

## 5. 通常の更新サイクル

```bash
cd /Users/kodamay/projects/xinu-rpi4/compile
make pi4
cp kernel8.img /opt/homebrew/var/tftp/
# Pi 4 電源 OFF/ON  ← SD swap 不要！
```

## 制約 / 注意

- 初期 EEPROM 設定 (step 3) は 1 回だけ必要だが Raspberry Pi OS 経由が必要.
- `dnsmasq` の sudo 必要 (UDP/67 bind のため).
- Mac の WiFi → Pi 4 が Ethernet → Mac の Internet Sharing 等が必要な場合あり.
- TFTP は UDP/69, セキュリティ的に LAN 内のみ.

## 簡易検証

```bash
# Pi 4 起動時に dnsmasq の log を tail
sudo tail -f /opt/homebrew/var/log/dnsmasq.log
# DHCPDISCOVER → DHCPOFFER → TFTP "get /start4.elf" 等が見えるはず
```
