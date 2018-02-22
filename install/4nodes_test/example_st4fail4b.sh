#!/bin/sh
ROUTECONF=pay_route.conf
AMOUNT=15000000
PAY_BEGIN=5555
PAY_END=4444

PAYER=node_${PAY_BEGIN}
PAYER_PORT=$(( ${PAY_BEGIN} + 1 ))
PAYEE=node_${PAY_END}
PAYEE_PORT=$(( ${PAY_END} + 1 ))

nodeid() {
	cat conf/peer$1.conf | awk '(NR==3) { print $1 }' | cut -d '=' -f2
}

./routing $PAYER/dbucoin `nodeid $PAY_BEGIN` `nodeid $PAY_END` $AMOUNT
if [ $? -ne 0 ]; then
	echo no routing
	exit -1
fi

INVOICE=`./ucoincli -i $AMOUNT $PAYEE_PORT`
if [ $? -ne 0 ]; then
	echo fail get invoice
	exit -1
fi
HASH=`echo $INVOICE | jq -r '.result.hash'`

./routing $PAYER/dbucoin `nodeid $PAY_BEGIN` `nodeid $PAY_END` $AMOUNT > $ROUTECONF

# fulfillしない
./ucoincli -d 1 $PAYEE_PORT

# 送金実施
./ucoincli -p $ROUTECONF,$HASH $PAYER_PORT

sleep 2

# 戻す
./ucoincli -d 0 $PAYEE_PORT