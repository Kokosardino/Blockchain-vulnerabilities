#!/bin/bash 

#MODIFY TO FIT YOUR SETUP!
#IP addresses  and passwords of devices.
export IP_ATTACKER="<INPUT_IP_ADDRESS>"
export IP_VICTIM1="<INPUT_IP_ADDRESS>"
export IP_VICTIM2="<INPUT_IP_ADDRESS>"
PASS_ATTACKER="<INPUT_PASSWORD>"
PASS_VICTIM2="<INPUT_PASSWORD>"

#Configure ssh.
(sshpass -p "$PASS_ATTACKER" ssh-copy-id -o "StrictHostKeyChecking=no" -i ~/.ssh/id_rsa.pub attacker@$IP_ATTACKER) &> /dev/null
(sshpass -p "$PASS_VICTIM2" ssh-copy-id -o "StrictHostKeyChecking=no" -i ~/.ssh/id_rsa.pub victim2@$IP_VICTIM2) &> /dev/null

#Create a fitting bitcoin.conf file, ports are by-default assigned as 18445.
cp ~/victim1/default_bitcoin.conf .bitcoin/bitcoin.conf
echo "bind=$IP_VICTIM1:18445" >> .bitcoin/bitcoin.conf
echo "addnode=$IP_ATTACKER:18445" >> .bitcoin/bitcoin.conf
echo "addnode=$IP_VICTIM2:18445" >> .bitcoin/bitcoin.conf

#Create a file to save finished transactions into.
touch ~/victim1/finishedTransactions.txt

#Start bitcoin daemon in regtest mode.
bitcoind -daemon 

#Wait for the start of the network.
sleep 1

#If a wallet exists, import it (we do not check default file location for wallet and create our own file "representing" it!). Otherwise, create new wallet.
if [ -f ~/victim1/wallet.out ]; then
	bitcoin-cli loadwallet $(cat ~/victim1/wallet.out | jq -r '.name') > /dev/null
else
    	bitcoin-cli createwallet "victim1_wallet" > ~/victim1/wallet.out
fi
