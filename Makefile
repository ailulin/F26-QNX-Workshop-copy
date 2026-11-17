PI_SSH_TARGET = qnxuser@10.37.87.53
REMOTE_DIR = /home/qnxuser
 
PULSE_EXE = 01-Pulse-Oximeter/build/aarch64le-debug/pulse-oximeter
ALARM_EXE = 02-Alarm-Manager/build/aarch64le-debug/alarm-manager
 
SSH = ssh -m hmac-sha2-256
SCP = scp -O -o MACs=hmac-sha2-256
 
.PHONY: all build deploy run clean
 
all: run
 
build:
	$(MAKE) -C 01-Pulse-Oximeter
	$(MAKE) -C 02-Alarm-Manager
 
deploy: build
	-$(SSH) $(PI_SSH_TARGET) "slay pulse-oximeter; slay alarm-manager"
	$(SCP) $(PULSE_EXE) $(ALARM_EXE) $(PI_SSH_TARGET):$(REMOTE_DIR)/
 
run: deploy
	$(SSH) $(PI_SSH_TARGET) "cd $(REMOTE_DIR); ./alarm-manager & sleep 1; ./pulse-oximeter"
 
clean:
	$(MAKE) -C 01-Pulse-Oximeter clean
	$(MAKE) -C 02-Alarm-Manager clean
