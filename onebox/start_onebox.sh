#!/bin/bash

# Copyright 2021 4Paradigm
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

set -x -e

cd "$(dirname "$0")/../"

ulimit -c unlimited

# the subdirectory can be set through the environment variable ONEBOX_WORKDIR
WORKSPACE=${ONEBOX_WORKDIR:-onebox/workspace}
rm -rf $WORKSPACE
mkdir -p $WORKSPACE

IP=127.0.0.1



function start_cluster() {
        # first start zookeeper
        ZK_CLUSTER=$IP:6181
        NS1=$IP:9622
        NS2=$IP:9623
        NS3=$IP:9624
        TABLET1=$IP:9520
        TABLET2=$IP:9521
        TABLET3=$IP:9522

        # start tablet0
        test -d $WORKSPACE/tablet0-binlogs && rm -rf $WORKSPACE/tablet0-binlogs
        test -d $WORKSPACE/recycle_bin0 && rm -rf $WORKSPACE/recycle_bin0
        ./build/bin/openmldb --db_root_path=$WORKSPACE/tablet0-binlogs \
                --recycle_bin_root_path=$WORKSPACE/recycle_bin0 \
                --endpoint="$TABLET1" --role=tablet \
                --openmldb_log_dir=$WORKSPACE/logs/tablet0 \
                --binlog_notify_on_put=true --zk_cluster="$ZK_CLUSTER" \
                --zk_keep_alive_check_interval=100000000 --zk_root_path=/onebox >$WORKSPACE/tablet0.log 2>&1 &
        sleep 2

        test -d $WORKSPACE/tablet1-binlogs && rm -rf $WORKSPACE/tablet1-binlogs
        test -d $WORKSPACE/recycle_bin1 && rm -rf $WORKSPACE/recycle_bin1
        # start tablet1
        ./build/bin/openmldb --db_root_path=$WORKSPACE/tablet1-binlogs \
                --recycle_bin_root_path=$WORKSPACE/recycle_bin1 \
                --endpoint="$TABLET2" --role=tablet \
                --openmldb_log_dir=$WORKSPACE/logs/tablet1 \
                --zk_cluster="$ZK_CLUSTER" \
                --binlog_notify_on_put=true --zk_keep_alive_check_interval=100000000 --zk_root_path=/onebox >$WORKSPACE/tablet1.log 2>&1 &
        sleep 2

        test -d $WORKSPACE/tablet2-binlogs && rm -rf $WORKSPACE/tablet2-binlogs
        test -d $WORKSPACE/recycle_bin2 && rm -rf $WORKSPACE/recycle_bin2
        # start tablet2
        ./build/bin/openmldb --db_root_path=$WORKSPACE/tablet2-binlogs \
                --recycle_bin_root_path=$WORKSPACE/recycle_bin2 \
                --endpoint="$TABLET3" --role=tablet \
                --openmldb_log_dir=$WORKSPACE/logs/tablet2 \
                --binlog_notify_on_put=true --zk_cluster="$ZK_CLUSTER" \
                --zk_keep_alive_check_interval=100000000 --zk_root_path=/onebox >$WORKSPACE/tablet2.log 2>&1 &
        sleep 2

        # start ns1
        ./build/bin/openmldb --endpoint="$NS1" --role=nameserver \
                --zk_cluster="$ZK_CLUSTER" \
                --openmldb_log_dir=$WORKSPACE/logs/ns1 \
                --tablet_offline_check_interval=1 --tablet_heartbeat_timeout=1 --zk_root_path=/onebox >$WORKSPACE/ns1.log 2>&1 &
        sleep 2

        # start ns2
        ./build/bin/openmldb --endpoint="$NS2" --role=nameserver \
                --zk_cluster="$ZK_CLUSTER" \
                --openmldb_log_dir=$WORKSPACE/logs/ns2 \
                --tablet_offline_check_interval=1 --tablet_heartbeat_timeout=1 --zk_root_path=/onebox >$WORKSPACE/ns2.log 2>&1 &
        sleep 2

        # start ns3
        ./build/bin/openmldb --endpoint="$NS3" --role=nameserver \
                --tablet_offline_check_interval=1 --openmldb_log_dir=$WORKSPACE/logs/ns3 \
                --tablet_heartbeat_timeout=1 --zk_cluster="$ZK_CLUSTER" \
                --zk_root_path=/onebox >$WORKSPACE/ns3.log 2>&1 &
        sleep 2
        echo "cluster start ok"
}

function start_standalone() {
        NS=$IP:6527
        TABLET=$IP:9921

        # start tablet
        test -d $WORKSPACE/tablet-binlogs && rm -rf $WORKSPACE/tablet-binlogs
        test -d $WORKSPACE/recycle_bin && rm -rf $WORKSPACE/recycle_bin
        ./build/bin/openmldb --db_root_path=$WORKSPACE/tablet-binlogs \
                --recycle_bin_root_path=$WORKSPACE/recycle_bin \
                --openmldb_log_dir=$WORKSPACE/logs/standalone-tb \
                --endpoint="$TABLET" --role=tablet \
                --binlog_notify_on_put=true >$WORKSPACE/tablet.log 2>&1 &
        sleep 2

        # start ns
        ./build/bin/openmldb --endpoint="$NS" --role=nameserver \
                --tablet="$TABLET" \
                --openmldb_log_dir=$WORKSPACE/logs/standalone-ns \
                --tablet_offline_check_interval=1 --tablet_heartbeat_timeout=1 >$WORKSPACE/ns.log 2>&1 &
        sleep 2
        echo "standalone start ok"
}
# with no param, default is cluster mode
if [ $# -eq 0 ]; then
        start_cluster
        exit 0
fi

if [ $# -ne 1 ]; then
        echo "parameter must be standalone|cluster"
        exit 1
fi

OP=$1
case $OP in
cluster)
        start_cluster
        ;;
standalone)
        start_standalone
        ;;
*)
        echo "only support standalone|cluster" >&2
        exit 1
        ;;
esac
