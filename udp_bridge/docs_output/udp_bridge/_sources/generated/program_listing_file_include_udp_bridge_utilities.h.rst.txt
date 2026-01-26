
.. _program_listing_file_include_udp_bridge_utilities.h:

Program Listing for File utilities.h
====================================

|exhale_lsh| :ref:`Return to documentation for file <file_include_udp_bridge_utilities.h>` (``include/udp_bridge/utilities.h``)

.. |exhale_lsh| unicode:: U+021B0 .. UPWARDS ARROW WITH TIP LEFTWARDS

.. code-block:: cpp

   #ifndef UDP_BRIDGE_UTILITIES_H
   #define UDP_BRIDGE_UTILITIES_H
   
   #include <string>
   
   namespace udp_bridge
   {
   
   std::string topicString(const std::string& topic, bool keep_slashes = false, char legal_first = 'r', char legal_char = '_');
   
   } // namespace udp_bridge
   
   #endif
