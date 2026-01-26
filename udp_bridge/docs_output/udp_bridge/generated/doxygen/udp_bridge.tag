<?xml version='1.0' encoding='UTF-8' standalone='yes' ?>
<tagfile doxygen_version="1.9.8">
  <compound kind="file">
    <name>packet.h</name>
    <path>include/udp_bridge/</path>
    <filename>packet_8h.html</filename>
    <class kind="struct">udp_bridge::PacketHeader</class>
    <class kind="struct">udp_bridge::Packet</class>
    <class kind="struct">udp_bridge::CompressedPacketHeader</class>
    <class kind="struct">udp_bridge::CompressedPacket</class>
    <class kind="struct">udp_bridge::FragmentHeader</class>
    <class kind="struct">udp_bridge::Fragment</class>
    <class kind="struct">udp_bridge::SequencedPacketHeader</class>
    <class kind="struct">udp_bridge::SequencedPacket</class>
  </compound>
  <compound kind="struct">
    <name>udp_bridge::CompressedPacket</name>
    <filename>structudp__bridge_1_1CompressedPacket.html</filename>
    <base>udp_bridge::CompressedPacketHeader</base>
  </compound>
  <compound kind="struct">
    <name>udp_bridge::CompressedPacketHeader</name>
    <filename>structudp__bridge_1_1CompressedPacketHeader.html</filename>
    <base>udp_bridge::PacketHeader</base>
    <member kind="variable">
      <type>uint32_t</type>
      <name>uncompressed_size</name>
      <anchorfile>structudp__bridge_1_1CompressedPacketHeader.html</anchorfile>
      <anchor>a708b9a56b83c81173b330466ec6d141c</anchor>
      <arglist></arglist>
    </member>
  </compound>
  <compound kind="class">
    <name>udp_bridge::Connection</name>
    <filename>classudp__bridge_1_1Connection.html</filename>
    <member kind="function">
      <type>std::pair&lt; double, double &gt;</type>
      <name>data_receive_rate</name>
      <anchorfile>classudp__bridge_1_1Connection.html</anchorfile>
      <anchor>a6cd0bc0ea006e92d4818f33775682e1e</anchor>
      <arglist>(double time)</arglist>
    </member>
    <member kind="function">
      <type>udp_bridge_interfaces::msg::DataRates</type>
      <name>data_sent_rate</name>
      <anchorfile>classudp__bridge_1_1Connection.html</anchorfile>
      <anchor>a293163149849570bc1467526322cca08</anchor>
      <arglist>(rclcpp::Time time, PacketSendCategory category)</arglist>
    </member>
    <member kind="function">
      <type>void</type>
      <name>cleanup_sent_packets</name>
      <anchorfile>classudp__bridge_1_1Connection.html</anchorfile>
      <anchor>a3e249986039936f96a7f73db5031c4c2</anchor>
      <arglist>(rclcpp::Time cutoff_time)</arglist>
    </member>
  </compound>
  <compound kind="class">
    <name>udp_bridge::ConnectionException</name>
    <filename>classudp__bridge_1_1ConnectionException.html</filename>
  </compound>
  <compound kind="struct">
    <name>udp_bridge::ConnectionRateInfo</name>
    <filename>structudp__bridge_1_1ConnectionRateInfo.html</filename>
  </compound>
  <compound kind="class">
    <name>udp_bridge::Defragmenter</name>
    <filename>classudp__bridge_1_1Defragmenter.html</filename>
    <member kind="function">
      <type>bool</type>
      <name>addFragment</name>
      <anchorfile>classudp__bridge_1_1Defragmenter.html</anchorfile>
      <anchor>a924c97ed9d63fa2a203386fc80670c3a</anchor>
      <arglist>(std::vector&lt; uint8_t &gt; fragment, rclcpp::Time now)</arglist>
    </member>
    <member kind="function">
      <type>std::vector&lt; std::vector&lt; uint8_t &gt; &gt;</type>
      <name>getPackets</name>
      <anchorfile>classudp__bridge_1_1Defragmenter.html</anchorfile>
      <anchor>a0eacba6debf06fe576519578cc8521ff</anchor>
      <arglist>()</arglist>
    </member>
    <member kind="function">
      <type>int</type>
      <name>cleanup</name>
      <anchorfile>classudp__bridge_1_1Defragmenter.html</anchorfile>
      <anchor>a4e71161b7f58ca68e34945c03c453259</anchor>
      <arglist>(rclcpp::Time discard_time)</arglist>
    </member>
  </compound>
  <compound kind="struct">
    <name>udp_bridge::Fragment</name>
    <filename>structudp__bridge_1_1Fragment.html</filename>
    <base>udp_bridge::FragmentHeader</base>
  </compound>
  <compound kind="struct">
    <name>udp_bridge::FragmentHeader</name>
    <filename>structudp__bridge_1_1FragmentHeader.html</filename>
    <base>udp_bridge::PacketHeader</base>
  </compound>
  <compound kind="struct">
    <name>udp_bridge::MessageSizeData</name>
    <filename>structudp__bridge_1_1MessageSizeData.html</filename>
    <member kind="variable">
      <type>int</type>
      <name>message_size</name>
      <anchorfile>structudp__bridge_1_1MessageSizeData.html</anchorfile>
      <anchor>ac164bb3dffe891b9e0311b4a2db76e72</anchor>
      <arglist></arglist>
    </member>
    <member kind="variable">
      <type>int</type>
      <name>fragment_count</name>
      <anchorfile>structudp__bridge_1_1MessageSizeData.html</anchorfile>
      <anchor>ad26325668a152cbe59203d0c46503732</anchor>
      <arglist></arglist>
    </member>
    <member kind="variable">
      <type>int</type>
      <name>sent_size</name>
      <anchorfile>structudp__bridge_1_1MessageSizeData.html</anchorfile>
      <anchor>a821b30e25cded00c3e7621c62bb8eaf6</anchor>
      <arglist></arglist>
    </member>
  </compound>
  <compound kind="class">
    <name>udp_bridge::MessageStatistics</name>
    <filename>classudp__bridge_1_1MessageStatistics.html</filename>
    <base>Statistics&lt; MessageSizeData &gt;</base>
  </compound>
  <compound kind="struct">
    <name>udp_bridge::Packet</name>
    <filename>structudp__bridge_1_1Packet.html</filename>
    <base>udp_bridge::PacketHeader</base>
  </compound>
  <compound kind="struct">
    <name>udp_bridge::PacketHeader</name>
    <filename>structudp__bridge_1_1PacketHeader.html</filename>
  </compound>
  <compound kind="class">
    <name>udp_bridge::PacketSendStatistics</name>
    <filename>classudp__bridge_1_1PacketSendStatistics.html</filename>
    <base>Statistics&lt; PacketSizeData &gt;</base>
  </compound>
  <compound kind="struct">
    <name>udp_bridge::PacketSizeData</name>
    <filename>structudp__bridge_1_1PacketSizeData.html</filename>
  </compound>
  <compound kind="struct">
    <name>udp_bridge::RemoteDetails</name>
    <filename>structudp__bridge_1_1RemoteDetails.html</filename>
  </compound>
  <compound kind="class">
    <name>udp_bridge::RemoteNode</name>
    <filename>classudp__bridge_1_1RemoteNode.html</filename>
  </compound>
  <compound kind="struct">
    <name>udp_bridge::SequencedPacket</name>
    <filename>structudp__bridge_1_1SequencedPacket.html</filename>
    <base>udp_bridge::SequencedPacketHeader</base>
  </compound>
  <compound kind="struct">
    <name>udp_bridge::SequencedPacketHeader</name>
    <filename>structudp__bridge_1_1SequencedPacketHeader.html</filename>
    <base>udp_bridge::PacketHeader</base>
  </compound>
  <compound kind="struct">
    <name>udp_bridge::SourceInfo</name>
    <filename>structudp__bridge_1_1SourceInfo.html</filename>
  </compound>
  <compound kind="class">
    <name>udp_bridge::Statistics</name>
    <filename>classudp__bridge_1_1Statistics.html</filename>
    <templarg>typename T</templarg>
  </compound>
  <compound kind="struct">
    <name>udp_bridge::SubscriberDetails</name>
    <filename>structudp__bridge_1_1SubscriberDetails.html</filename>
  </compound>
  <compound kind="class">
    <name>udp_bridge::UDPBridge</name>
    <filename>classudp__bridge_1_1UDPBridge.html</filename>
    <member kind="function">
      <type>void</type>
      <name>spin_once</name>
      <anchorfile>classudp__bridge_1_1UDPBridge.html</anchorfile>
      <anchor>a92143825f907e59f1568a03c789ead01</anchor>
      <arglist>()</arglist>
    </member>
  </compound>
  <compound kind="struct">
    <name>udp_bridge::WrappedPacket</name>
    <filename>structudp__bridge_1_1WrappedPacket.html</filename>
    <base>udp_bridge::SequencedPacketHeader</base>
  </compound>
  <compound kind="group">
    <name>packet</name>
    <title>UDP Packets</title>
    <filename>group__packet.html</filename>
    <class kind="struct">udp_bridge::Packet</class>
    <class kind="struct">udp_bridge::CompressedPacket</class>
    <class kind="struct">udp_bridge::Fragment</class>
    <class kind="struct">udp_bridge::SequencedPacket</class>
    <member kind="enumeration">
      <type></type>
      <name>udp_bridge::PacketType</name>
      <anchorfile>group__packet.html</anchorfile>
      <anchor>gab63281d979cbaf5d4220f8a6ef65aa8a</anchor>
      <arglist></arglist>
      <enumvalue file="group__packet.html" anchor="ggab63281d979cbaf5d4220f8a6ef65aa8aaf6068daa29dbb05a7ead1e3b5a48bbee">Data</enumvalue>
      <enumvalue file="group__packet.html" anchor="ggab63281d979cbaf5d4220f8a6ef65aa8aa4d602abc0c0f2f7c1a5156d964517e4e">Compressed</enumvalue>
      <enumvalue file="group__packet.html" anchor="ggab63281d979cbaf5d4220f8a6ef65aa8aaddc8ece84b3ddf895e62b85679d70e91">SubscribeRequest</enumvalue>
      <enumvalue file="group__packet.html" anchor="ggab63281d979cbaf5d4220f8a6ef65aa8aa37d01b98065725fe3a1d30acf3a0064a">Fragment</enumvalue>
      <enumvalue file="group__packet.html" anchor="ggab63281d979cbaf5d4220f8a6ef65aa8aac0b94d08da28fa4f82d8fd6fed87a57a">BridgeInfo</enumvalue>
      <enumvalue file="group__packet.html" anchor="ggab63281d979cbaf5d4220f8a6ef65aa8aad2977935c88b2edd6748b608be28a161">TopicStatistics</enumvalue>
      <enumvalue file="group__packet.html" anchor="ggab63281d979cbaf5d4220f8a6ef65aa8aaaa12765e5a91ab880f5759d38df463a3">WrappedPacket</enumvalue>
      <enumvalue file="group__packet.html" anchor="ggab63281d979cbaf5d4220f8a6ef65aa8aabdbd0bbd72d149068331e8145635706f">ResendRequest</enumvalue>
      <enumvalue file="group__packet.html" anchor="ggab63281d979cbaf5d4220f8a6ef65aa8aac2cc7082a89c1ad6631a2f66af5f00c0">Connection</enumvalue>
    </member>
  </compound>
</tagfile>
