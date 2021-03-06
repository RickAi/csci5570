
//
// Created by aiyongbiao on 2018/11/13.
//

#pragma once

#include <base/magic.hpp>
#include <base/context.hpp>
#include <base/third_party/general_fstream.hpp>
#include <iostream>
#include <io/hdfs_manager.hpp>
#include <vector>
#include <string>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/classification.hpp>
#include "boost/utility/string_ref.hpp"
#include "lib/parser.hpp"

namespace minips {

    class SVMDumper {
    public:
        // std::pair<std::vector<std::pair<int, double>>, double>
        void DumpSVMData(std::vector<SVMItem> data) {
            if (!Context::get_instance().get_bool("checkpoint_toggle")) {
                return;
            }
            auto dump_prefix = Context::get_instance().get_string("checkpoint_file_prefix");
            auto node_id = Context::get_instance().get_int32("my_id");
            auto dump_file = dump_prefix + "worker_" + std::to_string(node_id);
            LOG(INFO) << "Dump Data To: " << dump_file << ", Size=" << data.size();

            petuum::io::ofstream w_stream(dump_file, std::ofstream::out | std::ofstream::trunc);
            CHECK(w_stream);
            for (SVMItem item : data) {
                w_stream << item.second << " ";
                for (auto pair : item.first) {
                    w_stream << pair.first << ":" << pair.second << " ";
                }
                w_stream << "\n";
            }
            //w_stream << std::endl;
            w_stream.close();
        }

        int32_t RoundHundred(uint32_t input) {
            return 100 * round(input / 100.0);
        }

        void DumpConfigData(std::unordered_map<int, int> iter_map) {
            if (!Context::get_instance().get_bool("checkpoint_toggle")) {
                return;
            }
            auto dump_prefix = Context::get_instance().get_string("checkpoint_file_prefix");
            auto node_id = Context::get_instance().get_int32("my_id");
            auto dump_file = dump_prefix + "worker_config_" + std::to_string(node_id);
            LOG(INFO) << "Dump Config To: " << dump_file;

            petuum::io::ofstream w_stream(dump_file, std::ofstream::out | std::ofstream::trunc);
            CHECK(w_stream);
            for (auto it = iter_map.begin(); it != iter_map.end(); it++) {
                w_stream << it->first << ":" << RoundHundred(it->second) << " ";
            }
            w_stream.close();
        }

        std::unordered_map<int, int> LoadConfigData() {
            auto dump_prefix = Context::get_instance().get_string("checkpoint_file_prefix");
            auto node_id = Context::get_instance().get_int32("my_id");
            auto dump_file = dump_prefix + "worker_config_" + std::to_string(node_id);
            LOG(INFO) << "Load Config From: " << dump_file;

            std::unordered_map<int, int> result_map;
            petuum::io::ifstream input(dump_file.c_str());
            std::string line;
            while (std::getline(input, line)) {
                std::vector<std::string> tokens;
                boost::split(tokens, line, boost::is_any_of(" "));
                for (int i = 0; i < tokens.size(); i++) {
                    std::vector<std::string> pair_items;
                    boost::split(pair_items, tokens[i], boost::is_any_of(":"));
                    if (pair_items.size() < 2) {
                        continue;
                    }

                    int worker_id = std::atoi(pair_items[0].c_str());
                    int iter = std::atoi(pair_items[1].c_str());
                    result_map[worker_id] = iter;
                }
            }
            return result_map;
        }

        void DumpScaleFile(Node& node) {
            auto dump_file = Context::get_instance().get_string("scale_file");
            LOG(INFO) << "Dump Scale File Into: " << dump_file;

            petuum::io::ofstream w_stream(dump_file, std::ofstream::out | std::ofstream::trunc);
            CHECK(w_stream);
            w_stream << node.id << ":" << node.hostname << ":" << node.port;
            w_stream.close();
        }

        Node LoadScaleFile() {
            auto filename = Context::get_instance().get_string("scale_file");
            LOG(INFO) << "Load Scale File From: " << filename;

            petuum::io::ifstream input_file(filename.c_str());
            // CHECK(input_file.is_open()) << "Error opening file: " << filename;
            std::string line;
            getline(input_file, line);
            size_t id_pos = line.find(":");
            CHECK_NE(id_pos, std::string::npos);
            std::string id = line.substr(0, id_pos);
            size_t host_pos = line.find(":", id_pos + 1);
            CHECK_NE(host_pos, std::string::npos);
            std::string hostname = line.substr(id_pos + 1, host_pos - id_pos - 1);
            std::string port = line.substr(host_pos + 1, line.size() - host_pos - 1);

            Node node;
            node.id = std::stoi(id);
            node.hostname = std::move(hostname);
            node.port = std::stoi(port);
            return node;
        }

        std::vector<SVMItem> LoadSVMData(Node node, HDFSManager::Config config,
                                         std::vector<SVMItem> &datastore) {
            auto dump_prefix = Context::get_instance().get_string("checkpoint_raw_prefix");
            auto node_id = Context::get_instance().get_int32("my_id");
            auto dump_file = dump_prefix + "worker_" + std::to_string(node_id);
            LOG(INFO) << "Load Data From: " << dump_file;
            config.url = dump_file;
            config.master_host = node.hostname;

            zmq::context_t *zmq_context = new zmq::context_t(1);
            std::vector<Node> nodes;
            nodes.push_back(node);
            HDFSManager hdfs_manager(node, nodes, config, zmq_context);

            std::thread master_thread = std::thread([this, config, zmq_context] {
                HDFSBlockAssigner hdfs_block_assigner(config.hdfs_namenode, config.hdfs_namenode_port, zmq_context,
                                                      config.master_port);
                LOG(INFO) << "hdfs_block_assigner.Serve() start";
                hdfs_block_assigner.Serve();
                LOG(INFO) << "hdfs_block_assigner.Serve() end";
            });

            lib::Parser<SVMItem> parser;
            std::mutex lock;
            hdfs_manager.Run([this, node, &parser, &datastore, &lock](HDFSManager::InputFormat *input_format, int local_tid) {
                int count = 0;
//                LOG(INFO) << "start thread with tid" << local_tid;
                while (input_format->HasNext()) {
                    auto item = input_format->GetNextItem();
                    if (item.empty()) return;

                    // 3. Put samples into datastore
//                    auto data = parse(item);
                    auto data = parser.parse_libsvm(item);
                    lock.lock();
                    datastore.push_back(data);
                    count++;
                    lock.unlock();
                }
            });
            LOG(INFO) << "threads complete...";
            master_thread.join();
            LOG(INFO) << "Load Data Done With Size=" << datastore.size();

            return datastore;
        }

        SVMItem parse(boost::string_ref line) {
            SVMItem item;
            std::vector<std::string> tokens;
            boost::split(tokens, line, boost::is_any_of(" "));
            for (int i = 0; i < tokens.size(); i++) {
                if (i == 0) {
                    item.second = std::atof(tokens[i].c_str());
                } else {
                    std::vector<std::string> pair_items;
                    boost::split(pair_items, tokens[i], boost::is_any_of(":"));
                    if (pair_items.size() < 2) {
                        continue;
                    }

                    std::pair<int, double> pair_item;
                    pair_item.first = std::atoi(pair_items[0].c_str());;
                    pair_item.second = std::atof(pair_items[1].c_str());
                    item.first.push_back(pair_item);
                }
            }
            return item;
        }

    };

}
