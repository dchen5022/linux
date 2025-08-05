#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only

source ethtool-common.sh
lib_dir=$(dirname $0)/../../../net/
source $lib_dir/lib.sh

set -o pipefail

NSIM_NETDEV=$(make_netdev)

echo y > $NSIM_DEV_DFS/ethtool/mock_stats/enabled

stat=$(ethtool -S $NSIM_NETDEV | grep "hw_out_of_buffer" | awk '{print $2}')
((stat == 0))
check_code $? "ethtool stats show > 0 packets immediately after enabling"

sleep 2.5

stat=$(ethtool -S $NSIM_NETDEV | grep "hw_out_of_buffer" | awk '{print $2}')
((stat >= 20))
check_code $? "ethtool stats show < 20 packets after 2.5s passed"

echo n > $NSIM_DEV_DFS/ethtool/mock_stats/enabled

stat=$(ethtool -S $NSIM_NETDEV | grep "hw_out_of_buffer" | awk '{print $2}')
((stat == 0))
check_code $? "ethtool stats show > 0 packets after disabling"

if [ $num_errors -eq 0 ]; then
    echo "PASSED all $((num_passes)) checks"
    exit 0
else
    echo "FAILED $num_errors/$((num_errors+num_passes)) checks"
    exit 1
fi
