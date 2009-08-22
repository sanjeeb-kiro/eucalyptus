#include <stdio.h>
#include <stdlib.h>
#define __USE_GNU /* strnlen */
#include <string.h> /* strlen, strcpy */
#include <time.h>
#include <sys/types.h> /* fork */
#include <sys/wait.h> /* waitpid */
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h> /* SIGINT */

#include "ipc.h"
#include "misc.h"
#include <handlers.h>
#include <storage.h>
#include <eucalyptus.h>
#include <euca_auth.h>

/* coming from handlers.c */
extern sem * xen_sem;
extern sem * inst_sem;
extern bunchOfInstances * global_instances;

#define HYPERVISOR_URI "xen:///"

static int doInitialize (struct nc_state_t *nc) 
{
	logprintfl(EUCADEBUG, "doInitialized() invoked\n");

	/* set up paths of Eucalyptus commands NC relies on */
	snprintf (nc->gen_libvirt_cmd_path, CHAR_BUFFER_SIZE, EUCALYPTUS_GEN_LIBVIRT_XML, nc->home, nc->home);
	snprintf (nc->get_info_cmd_path, CHAR_BUFFER_SIZE, EUCALYPTUS_GET_XEN_INFO, nc->home, nc->home);
	snprintf (nc->virsh_cmd_path, CHAR_BUFFER_SIZE, EUCALYPTUS_VIRSH, nc->home);
	snprintf (nc->xm_cmd_path, CHAR_BUFFER_SIZE, EUCALYPTUS_XM);
	snprintf (nc->detach_cmd_path, CHAR_BUFFER_SIZE, EUCALYPTUS_DETACH, nc->home, nc->home);
	strcpy(nc->uri, HYPERVISOR_URI);
	nc->convert_to_disk = 0;

    return OK;
}

static int
getResources(	struct nc_state_t *nc,
		long long *cores,
		long long *memory){
	char *s = NULL;
	virNodeInfo ni;
	long long dom0_min_mem;

	if (virNodeGetInfo(nc->conn, &ni)) {
		logprintfl (EUCAFATAL, "error: failed to discover resources\n");
		return ERROR_FATAL;
	}

	/* dom0-min-mem has to come from xend config file */
	s = system_output (nc->get_info_cmd_path);
	if (get_value (s, "dom0-min-mem", &dom0_min_mem)) {
		logprintfl (EUCAFATAL, "error: did not find dom0-min-mem in output from %s\n", nc->get_info_cmd_path);
		free (s);
		return ERROR_FATAL;
	}
	free (s);

	/* calculate the available memory */
	*memory = ni.memory/1024 - 32 - dom0_min_mem;

	/* calculate the available cores */
	*cores = ni.cpus;

	return OK;
}



static int
doRunInstance(		struct nc_state_t *nc,
			ncMetadata *meta,
			char *instanceId,
			char *reservationId,
			ncInstParams *params, 
			char *imageId, char *imageURL, 
			char *kernelId, char *kernelURL, 
			char *ramdiskId, char *ramdiskURL, 
			char *keyName, 
			char *privMac, char *pubMac, int vlan, 
			char *userData, char *launchIndex,
			char **groupNames, int groupNamesSize,
			ncInstance **outInst)
{
    ncInstance * instance = NULL;
    * outInst = NULL;
    pid_t pid;
    ncNetConf ncnet;
    int error;

    strcpy(ncnet.privateMac, privMac);
    strcpy(ncnet.publicMac, pubMac);
    ncnet.vlan = vlan;

    /* check as much as possible before forking off and returning */
    sem_p (inst_sem);
    instance = find_instance (&global_instances, instanceId);
    sem_v (inst_sem);
    if (instance) {
        logprintfl (EUCAFATAL, "Error: instance %s already running\n", instanceId);
        return 1; /* TODO: return meaningful error codes? */
    }
    if (!(instance = allocate_instance (instanceId, 
                                        reservationId,
                                        params, 
                                        imageId, imageURL,
                                        kernelId, kernelURL,
                                        ramdiskId, ramdiskURL,
                                        instance_state_names[PENDING], 
                                        PENDING, 
                                        meta->userId, 
                                        &ncnet, keyName,
                                        userData, launchIndex, groupNames, groupNamesSize))) {
        logprintfl (EUCAFATAL, "Error: could not allocate instance struct\n");
        return 2;
    }
    instance->state = BOOTING; /* TODO: do this in allocate_instance()? */

    sem_p (inst_sem); 
    error = add_instance (&global_instances, instance);
    sem_v (inst_sem);
    if ( error ) {
        free_instance (&instance);
        logprintfl (EUCAFATAL, "Error: could not save instance struct\n");
        return error;
    }

    instance->launchTime = time (NULL);
    instance->params.memorySize = params->memorySize;
    instance->params.numberOfCores = params->numberOfCores;
    instance->params.diskSize = params->diskSize;
    strcpy (instance->ncnet.privateIp, "0.0.0.0");
    strcpy (instance->ncnet.publicIp, "0.0.0.0");

    /* do the potentially long tasks in a thread */
    pthread_attr_t* attr = (pthread_attr_t*) malloc(sizeof(pthread_attr_t));
    pthread_attr_init(attr);
    pthread_attr_setdetachstate(attr, PTHREAD_CREATE_DETACHED);
    
    if ( pthread_create (&(instance->tcb), attr, startup_thread, (void *)instance) ) {
        pthread_attr_destroy(attr);
        logprintfl (EUCAFATAL, "failed to spawn a VM startup thread\n");
        sem_p (inst_sem);
        remove_instance (&global_instances, instance);
        sem_v (inst_sem);
        free_instance (&instance);
        return 1;
    }
    pthread_attr_destroy(attr);

    * outInst = instance;
    return 0;

}

static int doRebootInstance(	struct nc_state_t *nc,
				ncMetadata *meta,
				char *instanceId) 
{
    ncInstance *instance;
    virConnectPtr *conn;

    sem_p (inst_sem); 
    instance = find_instance(&global_instances, instanceId);
    sem_v (inst_sem);
    if ( instance == NULL ) return NOT_FOUND;
    
    /* reboot the Xen domain */
    conn = check_hypervisor_conn();
    if (conn) {
        virDomainPtr dom = virDomainLookupByName(*conn, instanceId);
        if (dom) {
            /* also protect 'reboot', just in case */
            sem_p (xen_sem);
            int err=virDomainReboot (dom, 0);
            sem_v (xen_sem);
            if (err==0) {
                logprintfl (EUCAINFO, "rebooting Xen domain for instance %s\n", instanceId);
            }
            virDomainFree(dom); /* necessary? */
        } else {
            if (instance->state != BOOTING) {
                logprintfl (EUCAWARN, "warning: domain %s to be rebooted not running on hypervisor\n", instanceId);
            }
        }
    }

    return 0;
}

static int
doGetConsoleOutput(	struct nc_state_t *nc,
			ncMetadata *meta,
			char *instanceId,
			char **consoleOutput) {
  char *output;
  int pid, status, rc, bufsize, fd;
  char filename[1024];  

  if (getuid() != 0) {
    output = strdup("NOT SUPPORTED");
    *consoleOutput = base64_enc((unsigned char *)output, strlen(output));    
    if (output) free(output);
    return(0);
  }

  bufsize = sizeof(char) * 1024 * 64;
  output = malloc(bufsize);
  bzero(output, bufsize);

  snprintf(filename, 1024, "/tmp/consoleOutput.%s", instanceId);
  
  pid = fork();
  if (pid == 0) {
    int fd;
    fd = open(filename, O_WRONLY | O_TRUNC | O_CREAT, 0644);
    if (fd < 0) {
      // error
    } else {
      dup2(fd, 2);
      dup2(2, 1);
      close(0);
      // TODO: test virsh console:
      // rc = execl(rootwrap_command_path, rootwrap_command_path, "virsh", "console", instanceId, NULL);
      rc = execl("/usr/sbin/xm", "/usr/sbin/xm", "console", instanceId, NULL);
      fprintf(stderr, "execl() failed\n");
      close(fd);
    }
    exit(0);
  } else {
    int count;
    fd_set rfds;
    struct timeval tv;
    struct stat statbuf;
    
    count=0;
    while(count < 10000 && stat(filename, &statbuf) < 0) {count++;}
    fd = open(filename, O_RDONLY);
    if (fd < 0) {
      logprintfl (EUCAERROR, "ERROR: could not open consoleOutput file %s for reading\n", filename);
    } else {
      FD_ZERO(&rfds);
      FD_SET(fd, &rfds);
      tv.tv_sec = 0;
      tv.tv_usec = 500000;
      rc = select(1, &rfds, NULL, NULL, &tv);
      bzero(output, bufsize);
      
      count = 0;
      rc = 1;
      while(rc && count < 1000) {
	rc = read(fd, output, bufsize-1);
	count++;
      }
      close(fd);
    }
    kill(pid, 9);
    wait(&status);
  }
  
  unlink(filename);
  
  if (output[0] == '\0') {
    snprintf(output, bufsize, "EMPTY");
  }
  
  *consoleOutput = base64_enc((unsigned char *)output, strlen(output));
  free(output);
  
  return(0);
}

static int
doAttachVolume (	struct nc_state_t *nc,
			ncMetadata *meta,
			char *instanceId,
			char *volumeId,
			char *remoteDev,
			char *localDev)
{
    int ret = OK;
    ncInstance * instance;
    char localDevReal[32];
    virConnectPtr *conn;

    // fix up format of incoming local dev name, if we need to
    ret = convert_dev_names (localDev, localDevReal, NULL);
    if (ret)
        return ret;

    sem_p (inst_sem); 
    instance = find_instance(&global_instances, instanceId);
    sem_v (inst_sem);
    if ( instance == NULL ) 
        return NOT_FOUND;

    /* try attaching to the Xen domain */
    conn = check_hypervisor_conn();
    if (conn) {
        virDomainPtr dom = virDomainLookupByName(*conn, instanceId);
        if (dom) {

            int err = 0;
            char xml [1024];
            snprintf (xml, 1024, "<disk type='block'><driver name='phy'/><source dev='%s'/><target dev='%s'/></disk>", remoteDev, localDevReal);

            /* protect Xen calls, just in case */
            sem_p (xen_sem);
            err = virDomainAttachDevice (dom, xml);
            sem_v (xen_sem);
            if (err) {
                logprintfl (EUCAERROR, "AttachVolume() failed (err=%d) XML=%s\n", err, xml);
                ret = ERROR;
            } else {
                logprintfl (EUCAINFO, "attached %s to %s in domain %s\n", remoteDev, localDevReal, instanceId);
            }
            virDomainFree(dom);
        } else {
            if (instance->state != BOOTING) {
                logprintfl (EUCAWARN, "warning: domain %s not running on hypervisor, cannot attach device\n", instanceId);
            }
            ret = ERROR;
        }
    } else {
        ret = ERROR;
    }

    if (ret==OK) {
        ncVolume * volume;

        sem_p (inst_sem);
        volume = add_volume (instance, volumeId, remoteDev, localDevReal);
	scSaveInstanceInfo(instance); /* to enable NC recovery */
        sem_v (inst_sem);
        if ( volume == NULL ) {
            logprintfl (EUCAFATAL, "ERROR: Failed to save the volume record, aborting volume attachment\n");
            return ERROR;
        }
    }

    return ret;
}

static int
doDetachVolume (	struct nc_state_t *nc,
			ncMetadata *meta,
			char *instanceId,
			char *volumeId,
			char *remoteDev,
			char *localDev,
			int force)
{
    int ret = OK;
    ncInstance * instance;
    char localDevReal[32];
    virConnectPtr *conn;

    // fix up format of incoming local dev name, if we need to
    ret = convert_dev_names (localDev, localDevReal, NULL);
    if (ret)
        return ret;

    sem_p (inst_sem); 
    instance = find_instance(&global_instances, instanceId);
    sem_v (inst_sem);
    if ( instance == NULL ) 
        return NOT_FOUND;

    /* try attaching to the Xen domain */
    conn = check_hypervisor_conn(); 
    if (conn) {
        virDomainPtr dom = virDomainLookupByName(*conn, instanceId);
        if (dom) {
	    int err = 0, fd, rc, pid, status;
            char xml [1024], tmpfile[32], cmd[1024];
	    FILE *FH;
	    
            snprintf (xml, 1024, "<disk type='block'><driver name='phy'/><source dev='%s'/><target dev='%s'/></disk>", remoteDev, localDevReal);

            /* protect Xen calls, just in case */
            sem_p (xen_sem);
	    pid = fork();
	    if (!pid) {
	      char cmd[1024];
	      snprintf(tmpfile, 32, "/tmp/detachxml.XXXXXX");
	      fd = mkstemp(tmpfile);
	      if (fd > 0) {
		write(fd, xml, strlen(xml));
		close(fd);
		snprintf(cmd, 1024, "%s %s `which virsh` %s %s %s", nc->detach_cmd_path, nc->rootwrap_cmd_path, instanceId, localDevReal, tmpfile);
		rc = system(cmd);
		rc = rc>>8;
		unlink(tmpfile);
	      } else {
		logprintfl(EUCAERROR, "could not write to tmpfile for detach XML: %s\n", tmpfile);
		rc = 1;
	      } 
	      exit(rc);
	    } else {
	      rc = timewait(pid, &status, 10);
	      if (WEXITSTATUS(status)) {
		logprintfl(EUCAERROR, "failed to sucessfully run detach helper\n");
		err = 1;
	      } else {
		err = 0;
	      }
	    }
#if 0
	    if (!getuid()) {
	      err = virDomainDetachDevice (dom, xml);
	    } else {
	      
	      /* virsh detach function does not work as non-root user on xen (bug). workaround is to shellout to virsh */
	      snprintf(tmpfile, 32, "/tmp/detachxml.XXXXXX");
	      fd = mkstemp(tmpfile);
	      if (fd > 0) {
		write(fd, xml, strlen(xml));
		close(fd);
		snprintf(cmd, 1024, "%s detach-device %s %s",virsh_command_path, instanceId, tmpfile);
		logprintfl(EUCADEBUG, "Running command: %s\n", cmd);
		err = WEXITSTATUS(system(cmd));
		unlink(tmpfile);
		if (err) {
		  logprintfl(EUCADEBUG, "first workaround command failed (%d), trying second workaround...\n", err);
		  snprintf(cmd, 1024, "%s block-detach %s %s", xm_command_path, instanceId, localDevReal);
		  logprintfl(EUCADEBUG, "Running command: %s\n", cmd);
		  err = WEXITSTATUS(system(cmd));
		}
	      } else {
		err = 1;
	      }
	    }
#endif
            sem_v (xen_sem);
	    
            if (err) {
                logprintfl (EUCAERROR, "DetachVolume() failed (err=%d) XML=%s\n", err, xml);
                ret = ERROR;
            } else {
                logprintfl (EUCAINFO, "detached %s as %s in domain %s\n", remoteDev, localDevReal, instanceId);
            }
            virDomainFree(dom);
	} else {
            if (instance->state != BOOTING) {
                logprintfl (EUCAWARN, "warning: domain %s not running on hypervisor, cannot detach device\n", instanceId);
            }
            ret = ERROR;
        }
    } else {
        ret = ERROR;
    }

    if (ret==OK) {
        ncVolume * volume;

        sem_p (inst_sem);
        volume = free_volume (instance, volumeId, remoteDev, localDevReal);
        sem_v (inst_sem);
        if ( volume == NULL ) {
            logprintfl (EUCAFATAL, "ERROR: Failed to find and remove volume record, aborting volume detachment\n");
            return ERROR;
        }
    }

    return ret;
}

struct handlers xen_libvirt_handlers = {
    .name = "xen",
    .doInitialize        = doInitialize,
    .getResources        = getResources,
    .doDescribeInstances = NULL,
    .doRunInstance       = doRunInstance,
    .doTerminateInstance = NULL,
    .doRebootInstance    = doRebootInstance,
    .doGetConsoleOutput  = doGetConsoleOutput,
    .doDescribeResource  = NULL,
    .doStartNetwork      = NULL,
    .doPowerDown         = NULL,
    .doAttachVolume      = doAttachVolume,
    .doDetachVolume      = doDetachVolume
};

