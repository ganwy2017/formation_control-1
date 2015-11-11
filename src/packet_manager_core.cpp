/*  Copyright (C) 2015 Alessandro Tondo
 *  email: tondo.codes+ros <at> gmail.com
 *
 *  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public 
 *  License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any 
 *  later version.
 *  This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied 
 *  warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more 
 *  details.
 *  You should have received a copy of the GNU General Public License along with this program.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include "packet_manager_core.h"

PacketManagerCore::PacketManagerCore() {
  // handles server private parameters (private names are protected from accidental name collisions)
  private_node_handle_ = new ros::NodeHandle("~");

  std::string serial_port;
  int serial_baudrate;
  private_node_handle_->param("sample_time", sample_time_, (double)DEFAULT_SAMPLE_TIME);
  private_node_handle_->param("verbosity_level", verbosity_level_, DEFAULT_VERBOSITY_LEVEL);
  private_node_handle_->param("serial_port", serial_port, (std::string)DEFAULT_SERIAL_PORT);
  private_node_handle_->param("serial_baudrate", serial_baudrate, DEFAULT_SERIAL_BAUDRATE);
  private_node_handle_->param("serial_timeout", serial_timeout_, (double)DEFAULT_SERIAL_TIMEOUT);
  private_node_handle_->param("buffer_length", buffer_length_, DEFAULT_BUFFER_LENGTH);

  private_node_handle_->param("topic_queue_length", topic_queue_length_, DEFAULT_TOPIC_QUEUE_LENGTH);
  private_node_handle_->param("shared_stats_topic", shared_stats_topic_name_, std::string(DEFAULT_SHARED_STATS_TOPIC));
  private_node_handle_->param("received_stats_topic", received_stats_topic_name_, std::string(DEFAULT_RECEIVED_STATS_TOPIC));
  private_node_handle_->param("target_stats_topic", target_stats_topic_name_, std::string(DEFAULT_TARGET_STATS_TOPIC));
  private_node_handle_->param("agent_poses_topic", agent_poses_topic_name_, std::string(DEFAULT_AGENT_POSES_TOPIC));

  private_node_handle_->param("frame_map", frame_map_, std::string(DEFAULT_FRAME_MAP));
  private_node_handle_->param("frame_agent_prefix", frame_agent_prefix_, std::string(DEFAULT_FRAME_AGENT_PREFIX));
  private_node_handle_->param("frame_virtual_suffix", frame_virtual_suffix_, std::string(DEFAULT_FRAME_VIRTUAL_SUFFIX));

  try {
    // by default the constructor sets 8 bits data, no parity, 1 stop bit and no flow control, while serial::Timeout(0)
    // means non-blocking read/write primitives
    serial_ = new serial::Serial(serial_port, (uint32_t)serial_baudrate, serial::Timeout(0));
  }
  catch (const serial::IOException& e) {
    std::stringstream s;
    s << "Can't open a serial communication on port '" << serial_port << "'.\n" << e.what();
    console(__func__, s, FATAL);

    s << "Available ports:";
    std::vector<serial::PortInfo> available_port_list = serial::list_ports();
    for (auto const &port_info : available_port_list) {
      s << "\n        + " << port_info.port;
    }
    console(__func__, s, WARN);

    ros::shutdown();
    std::exit(EXIT_FAILURE);
  }

  serial_->flush();  // flushes both read and write streams

  // packet manager primitives (see ./src/packet_manager.c)
//  pm_init(std::bind(&PacketManagerCore::newPacket, this, std::placeholders::_1), PacketManagerCore::errorDeserialize, PacketManagerCore::errorSerialize);
//  pm_register_packet(PCK_TARGET, target_statistics_serialize, target_statistics_deserialize, target_statistics_reset);
//  pm_register_packet(PCK_RECEIVED, received_statistics_serialize, received_statistics_deserialize, received_statistics_reset);
  pm_register_packet(PCK_AGENT, agent_serialize, agent_deserialize, agent_reset);


  stats_publisher_ = node_handle_.advertise<agent_test::FormationStatisticsStamped>(shared_stats_topic_name_, topic_queue_length_);
  stats_subscriber_ = node_handle_.subscribe(received_stats_topic_name_, topic_queue_length_, &PacketManagerCore::receivedStatsCallback, this);
  target_stats_subscriber_ = node_handle_.subscribe(target_stats_topic_name_, topic_queue_length_, &PacketManagerCore::targetStatsCallback, this);
  agent_poses_publisher_ = node_handle_.advertise<geometry_msgs::PoseStamped>(agent_poses_topic_name_, topic_queue_length_);

  algorithm_timer_ = private_node_handle_->createTimer(ros::Duration(sample_time_), &PacketManagerCore::algorithmCallback, this);
  time_last_packet_ = ros::Time::now();
}

PacketManagerCore::~PacketManagerCore() {
  serial_->close();
  delete serial_;
  delete private_node_handle_;
}

void PacketManagerCore::algorithmCallback(const ros::TimerEvent &timer_event) {
  ros::Duration duration_last_packet = ros::Time::now() - time_last_packet_;
  if (duration_last_packet > ros::Duration(serial_timeout_)) {
    std::stringstream s;
    s << "Serial communication timeout occurred (last packet received " << duration_last_packet << " seconds ago).";
    console(__func__, s, FATAL);

    ros::shutdown();
    std::exit(EXIT_FAILURE);
  }

  if (packet_queue_.empty()) {
    // receives data from serial interface (retrieved packets will be inserted in the proper queue)
    serialReceivePacket();
    return;
  }

  // there is at least one pending packet
  processPackets();
  time_last_packet_ = ros::Time::now();
}

void PacketManagerCore::console(const std::string &caller_name, std::stringstream &message, const int &log_level) const {
  std::stringstream s;
  s << "[PacketManagerCore::" << caller_name << "]  " << message.str();

  if (log_level < WARN) {  // error messages
    ROS_ERROR_STREAM(s.str());
  }
  else if (log_level == WARN) {  // warn messages
    ROS_WARN_STREAM(s.str());
  }
  else if (log_level == INFO) {  // info messages
    ROS_INFO_STREAM(s.str());
  }
  else if (log_level > INFO && log_level <= verbosity_level_) {  // debug messages
    ROS_DEBUG_STREAM(s.str());
  }

  // clears the stringstream passed to this method
  message.clear();
  message.str(std::string());
}

void PacketManagerCore::errorDeserialize(unsigned char header, unsigned char errno) {
//  if (header == 1 && errno == 0) {
//
//  }
}

void PacketManagerCore::errorSerialize(unsigned char header, unsigned char errno) {
//  if (header == 1 && errno == 0) {
//
//  }
}

void PacketManagerCore::newPacket(unsigned char header) {
  if (header == PCK_AGENT) {
    packet_queue_.push(agent_data);
  }
}

void PacketManagerCore::processPackets() {
  // received packets are always of type Agent (see ./include/packet_manager/packet_agent.h)
  Agent packet = packet_queue_.front();
  packet_queue_.pop();

  std::string agent_frame = frame_agent_prefix_ + std::to_string((int)packet.agent_id);
  std::string agent_frame_virtual = agent_frame + frame_virtual_suffix_;

  agent_test::FormationStatisticsStamped msg_estimated_statistics;
  msg_estimated_statistics.agent_id = (int)packet.agent_id;
  msg_estimated_statistics.header.frame_id = agent_frame_virtual;
  msg_estimated_statistics.header.stamp = ros::Time::now();
  msg_estimated_statistics.stats.m_x = (double)packet.stats.m_x;
  msg_estimated_statistics.stats.m_y = (double)packet.stats.m_y;
  msg_estimated_statistics.stats.m_xx = (double)packet.stats.m_xx;
  msg_estimated_statistics.stats.m_xy = (double)packet.stats.m_xy;
  msg_estimated_statistics.stats.m_yy = (double)packet.stats.m_yy;

  geometry_msgs::PoseStamped  msg_pose;
  msg_pose.header.frame_id = agent_frame;
  msg_pose.header.stamp = ros::Time::now();
  msg_pose.pose = setPose((double)packet.pose_x, (double)packet.pose_y, (double)packet.pose_theta);

  geometry_msgs::PoseStamped  msg_pose_virtual;
  msg_pose_virtual.header.frame_id = agent_frame_virtual;
  msg_pose_virtual.header.stamp = ros::Time::now();
  msg_pose_virtual.pose = setPose((double)packet.pose_x_virtual, (double)packet.pose_y_virtual, (double)packet.pose_theta_virtual);

  stats_publisher_.publish(msg_estimated_statistics);
  agent_poses_publisher_.publish(msg_pose);
  agent_poses_publisher_.publish(msg_pose_virtual);

  std::stringstream s;
  s << "Received data from " << agent_frame << ".";
  console(__func__, s, INFO);
  s << agent_frame << " estimated statistics (" << (double)packet.stats.m_x << ", " << (double)packet.stats.m_y << ", "
    << (double)packet.stats.m_xx << ", " << (double)packet.stats.m_xy << ", " << (double)packet.stats.m_yy << ").";
  console(__func__, s, DEBUG_VVV);
  s << agent_frame << " pose (" << (double)packet.pose_x << ", " << (double)packet.pose_y << ", "
                                << (double)packet.pose_theta << ").";
  console(__func__, s, DEBUG_VVV);
  s << agent_frame_virtual << " pose (" << (double)packet.pose_x_virtual << ", " << (double)packet.pose_y_virtual << ", "
                                        << (double)packet.pose_theta_virtual << ").";
  console(__func__, s, DEBUG_VVV);
}

void PacketManagerCore::receivedStatsCallback(const agent_test::FormationStatisticsArray &received) {
  received_statistics_data.stats_sum.m_x = 0;
  received_statistics_data.stats_sum.m_y = 0;
  received_statistics_data.stats_sum.m_xx = 0;
  received_statistics_data.stats_sum.m_xy = 0;
  received_statistics_data.stats_sum.m_yy = 0;

  // TODO: modify the MATLAB scheme to fit this packet
  received_statistics_data.number_of_agents = (i_uint8)received.vector.size();
  for (auto const &data : received.vector) {
    received_statistics_data.stats_sum.m_x += (i_float)data.stats.m_x;
    received_statistics_data.stats_sum.m_y += (i_float)data.stats.m_y;
    received_statistics_data.stats_sum.m_xx += (i_float)data.stats.m_xx;
    received_statistics_data.stats_sum.m_xy += (i_float)data.stats.m_xy;
    received_statistics_data.stats_sum.m_yy += (i_float)data.stats.m_yy;
  }

  // sends data to serial interface
  serialSendPacket(PCK_RECEIVED);

  std::stringstream s;
  s << "Received statistics from " << received.vector.size()  << " other agents.";
  console(__func__, s, DEBUG);
}

void PacketManagerCore::serialReceivePacket() {
  uint8_t buffer[buffer_length_];
  size_t bytes_read;

  do {
    bytes_read = serial_->read(buffer, sizeof(buffer));

    std::stringstream s;
    s << "Received " << bytes_read  << " bytes of data from the serial communication.";
    console(__func__, s, DEBUG);

    if (bytes_read > 0) {
      for (int i=0; i<bytes_read; i++)
        _pm_process_byte(buffer[i]);
    }
  } while (bytes_read != 0);
}

void PacketManagerCore::serialSendPacket(unsigned char header) {
  char sent_status;
  short ch;
  uint8_t buffer[buffer_length_];
  uint32_t id = 0;

  do {
    ch = _pm_send_byte(header, &sent_status);
    if (ch != -200) {  // hard coded (see ./src/packet_manager/packet_manager.c)
      buffer[id++] = (uint8_t)ch;
    }
  } while (sent_status == 0 && ch != -200);

  size_t bytes_sent = (size_t)id;  // id equals the length of the data to be sent
  serial_->write(buffer, bytes_sent);

  std::stringstream s;
  s << "Sent " << bytes_sent  << " bytes of data over the serial communication.";
  console(__func__, s, DEBUG);
  if (bytes_sent > buffer_length_) {
    s << "The number of sent bytes exceeds the buffer length (" << buffer_length_ << ").";
    console(__func__, s, ERROR);
  }
}

geometry_msgs::Pose PacketManagerCore::setPose(const double &x, const double &y, const double &theta) const {
  tf::Pose pose_tf;
  geometry_msgs::Pose  pose_msg;

  pose_tf.setOrigin(tf::Vector3(x, y, 0));
  pose_tf.setRotation(tf::createQuaternionFromRPY(0, 0, theta));
  poseTFToMsg(pose_tf, pose_msg);

  return pose_msg;
}

void PacketManagerCore::targetStatsCallback(const agent_test::FormationStatisticsStamped &target) {
  target_statistics_data.stats.m_x = (i_float)target.stats.m_x;
  target_statistics_data.stats.m_y = (i_float)target.stats.m_y;
  target_statistics_data.stats.m_xx = (i_float)target.stats.m_xx;
  target_statistics_data.stats.m_xy = (i_float)target.stats.m_xy;
  target_statistics_data.stats.m_yy = (i_float)target.stats.m_yy;

  // sends data to serial interface
  serialSendPacket(PCK_TARGET);

  std::stringstream s;
  s << "Target statistics has been changed.";
  console(__func__, s, INFO);
  s << "New target statistics (" << target.stats.m_x << ", " << target.stats.m_y << ", " << target.stats.m_xx << ", "
    << target.stats.m_xy << ", " << target.stats.m_yy << ").";
  console(__func__, s, DEBUG_VVVV);
}