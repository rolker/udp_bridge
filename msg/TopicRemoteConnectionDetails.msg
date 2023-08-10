# Part of TopicRemoteDetails specifing details about a specific connection to a remote 

# connection id
string  connection_id

# Optional rate limit for sending topic if positive, topic disabled if negative.
# No rate limit if zero, otherwise, period is
# time in seconds to wait between sending messages from this topic.
float32 period
