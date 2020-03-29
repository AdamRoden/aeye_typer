#!/bin/bash

# Runs and enters the docker container given by `app_name` with the app's local
# codebase and data directories mounted, along with the user's local home
# directory.
# Note: Exiting the container does not kill it.  

app_name="fast_aeye_typer"  # Your docker image name


# Set up cmd line args
local_data_dir=$1
local_codebase_dir=$(pwd)
local_home_dir=$(echo ~)


if [[ -z $local_data_dir ]]
then 
  local_data_dir="/data/$app_name"
  echo "WARN: Missing argument 1 (data folder)... Using $local_data_dir instead."
fi

echo "INFO: Mounting $local_data_dir at /opt/app/data."
echo "INFO: Mounting $local_codebase_dir at /opt/app/src."
echo "INFO: Mounting $local_home_dir at /opt/home."

# Set .ssh dir
local_ssh_dir="$local_home_dir/.ssh"

# Allow local x win connections for users in the docker group
xhost +local:docker

# Start detached docker contnainer w/local directories mounted
container_name="$app_name"_"$$"

nvidia-docker run --rm -it -d \
	--name $container_name \
  -v $local_data_dir:/opt/app/data \
  -v $local_codebase_dir:/opt/app/src \
  -v $local_home_dir:/opt/home \
  -v $local_ssh_dir:/root/.ssh \
	-v /tmp/.X11-unix:/tmp/.X11-unix \
	-e DISPLAY=$DISPLAY \
	--net=host \
  --ipc=host \
	--runtime=nvidia \
	--privileged \
  $app_name
  
  # Exec into the detached container
  docker exec -it $container_name bash