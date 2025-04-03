from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration
from launch.substitutions import PythonExpression
from launch_ros.actions import LifecycleNode
from launch_ros.actions import LifecycleTransition

from lifecycle_msgs.msg import Transition

def generate_launch_description():
    name = LaunchConfiguration('name')
    port = LaunchConfiguration('port')
    maximum_packet_size = LaunchConfiguration('maximum_packet_size')

    name_arg = DeclareLaunchArgument('name', default_value='udp_bridge')
    port_arg = DeclareLaunchArgument('port', default_value='0')
    maximum_packet_size_arg = DeclareLaunchArgument('maximum_packet_size', default_value='0')

    return LaunchDescription([
        name_arg,
        port_arg,
        maximum_packet_size_arg,
        LifecycleNode(
            package='udp_bridge',
            executable='udp_bridge_node',
            name='udp_bridge',
            namespace='',
            respawn=True,
            respawn_delay=2,
            parameters=[{
                'name': name,
                'port': port,
                'maximum_packet_size': maximum_packet_size,
            }],
        ),
        LifecycleTransition(
            lifecycle_node_names=(
                PythonExpression(
                    expression = [
                        '"',
                        LaunchConfiguration("ros_namespace", default=''),
                        '" + "/udp_bridge"'
                    ],
                ),
            ),
            transition_ids=(
                Transition.TRANSITION_CONFIGURE,
                Transition.TRANSITION_ACTIVATE,
            )
        ),
    ])
