#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#include <asterisk/lock.h>
#include <asterisk/channel.h>
#include <asterisk/logger.h>
#include <asterisk/file.h>
#include <asterisk/pbx.h>
#include <asterisk/module.h>
#include <asterisk/manager.h>
#include <asterisk/cli.h>
#include <asterisk/monitor.h>

#define AST_MONITOR_DIR	INSTALL_PREFIX "/var/spool/asterisk/monitor"

static ast_mutex_t monitorlock = AST_MUTEX_INITIALIZER;

static unsigned long seq = 0;

static char *monitor_synopsis = "Monitor a channel";

static char *monitor_descrip = "Monitor\n"
	"Used to start monitoring a channel. The channel's input and output\n"
	"voice packets are logged to files until the channel hangs up or\n"
	"monitoring is stopped by the StopMonitor application.\n"
	"The option string may contain the following arguments: [file_format|[fname_base]]\n"
	"	file_format -- optional, if not set, defaults to \"wav\"\n"
	"	fname_base -- if set, changes the filename used to the one specified.\n";

static char *stopmonitor_synopsis = "Stop monitoring a channel";

static char *stopmonitor_descrip = "StopMonitor\n"
	"Stops monitoring a channel. Has no effect if the channel is not monitored\n";

static char *changemonitor_synopsis = "Change monitoring filename of a channel";

static char *changemonitor_descrip = "ChangeMonitor\n"
	"Changes monitoring filename of a channel. Has no effect if the channel is not monitored\n"
	"The option string may contain the following:\n"
	"	filename_base -- if set, changes the filename used to the one specified.\n";

/* Start monitoring a channel */
int ast_monitor_start(	struct ast_channel *chan, const char *format_spec,
						const char *fname_base, int need_lock )
{
	int res = 0;

	if( need_lock ) {
		if (ast_mutex_lock(&chan->lock)) {
			ast_log(LOG_WARNING, "Unable to lock channel\n");
			return -1;
		}
	}

	if( !(chan->monitor) ) {
		struct ast_channel_monitor *monitor;
		char *channel_name, *p;

		/* Create monitoring directory if needed */
		if( mkdir( AST_MONITOR_DIR, 0770 ) < 0 ) {
			if( errno != EEXIST ) {
				ast_log(LOG_WARNING, "Unable to create audio monitor directory: %s\n",
						strerror( errno ) );
			}
		}

		monitor = malloc( sizeof( struct ast_channel_monitor ) );
		memset( monitor, 0, sizeof( struct ast_channel_monitor ) );

		/* Determine file names */
		if( fname_base && strlen( fname_base ) ) {
			snprintf(	monitor->read_filename, FILENAME_MAX, "%s/%s-in",
						AST_MONITOR_DIR, fname_base );
			snprintf(	monitor->write_filename, FILENAME_MAX, "%s/%s-out",
						AST_MONITOR_DIR, fname_base );
			*monitor->filename_base = 0;
		} else {
			ast_mutex_lock( &monitorlock );
			snprintf(	monitor->read_filename, FILENAME_MAX, "%s/audio-in-%ld",
						AST_MONITOR_DIR, seq );
			snprintf(	monitor->write_filename, FILENAME_MAX, "%s/audio-out-%ld",
						AST_MONITOR_DIR, seq );
			seq++;
			ast_mutex_unlock( &monitorlock );

			channel_name = strdup( chan->name );
			while( ( p = strchr( channel_name, '/' ) ) ) {
				*p = '-';
			}
			snprintf(	monitor->filename_base, FILENAME_MAX, "%s/%s",
						AST_MONITOR_DIR, channel_name );
			free( channel_name );
		}

		monitor->stop = ast_monitor_stop;

		// Determine file format
		if( format_spec && strlen( format_spec ) ) {
			monitor->format = strdup( format_spec );
		} else {
			monitor->format = strdup( "wav" );
		}
		
		// open files
		if( ast_fileexists( monitor->read_filename, NULL, NULL ) > 0 ) {
			ast_filedelete( monitor->read_filename, NULL );
		}
		if( !(monitor->read_stream = ast_writefile(	monitor->read_filename,
						monitor->format, NULL,
						O_CREAT|O_TRUNC|O_WRONLY, 0, 0644 ) ) ) {
			ast_log(	LOG_WARNING, "Could not create file %s\n",
						monitor->read_filename );
			free( monitor );
			ast_mutex_unlock(&chan->lock);
			return -1;
		}
		if( ast_fileexists( monitor->write_filename, NULL, NULL ) > 0 ) {
			ast_filedelete( monitor->write_filename, NULL );
		}
		if( !(monitor->write_stream = ast_writefile( monitor->write_filename,
						monitor->format, NULL,
						O_CREAT|O_TRUNC|O_WRONLY, 0, 0644 ) ) ) {
			ast_log(	LOG_WARNING, "Could not create file %s\n",
						monitor->write_filename );
			ast_closestream( monitor->read_stream );
			free( monitor );
			ast_mutex_unlock(&chan->lock);
			return -1;
		}
		chan->monitor = monitor;
	} else {
		ast_log(	LOG_DEBUG,"Cannot start monitoring %s, already monitored\n",
					chan->name );
		res = -1;
	}

	if( need_lock ) {
		ast_mutex_unlock(&chan->lock);
	}
	return res;
}

/* Stop monitoring a channel */
int ast_monitor_stop( struct ast_channel *chan, int need_lock )
{
	if(need_lock) {
		if(ast_mutex_lock(&chan->lock)) {
			ast_log(LOG_WARNING, "Unable to lock channel\n");
			return -1;
		}
	}

	if(chan->monitor) {
		char filename[ FILENAME_MAX ];

		if(chan->monitor->read_stream) {
			ast_closestream( chan->monitor->read_stream );
		}
		if(chan->monitor->write_stream) {
			ast_closestream( chan->monitor->write_stream );
		}

		if(chan->monitor->filename_base&&strlen(chan->monitor->filename_base)) {
			if( ast_fileexists(chan->monitor->read_filename,NULL,NULL) > 0 ) {
				snprintf(	filename, FILENAME_MAX, "%s-in",
							chan->monitor->filename_base );
				if(ast_fileexists( filename, NULL, NULL ) > 0) {
					ast_filedelete( filename, NULL );
				}
				ast_filerename(	chan->monitor->read_filename, filename,
								chan->monitor->format );
			} else {
				ast_log(	LOG_WARNING, "File %s not found\n",
							chan->monitor->read_filename );
			}

			if(ast_fileexists(chan->monitor->write_filename,NULL,NULL) > 0) {
				snprintf(	filename, FILENAME_MAX, "%s-out",
							chan->monitor->filename_base );
				if( ast_fileexists( filename, NULL, NULL ) > 0 ) {
					ast_filedelete( filename, NULL );
				}
				ast_filerename(	chan->monitor->write_filename, filename,
								chan->monitor->format );
			} else {
				ast_log(	LOG_WARNING, "File %s not found\n",
							chan->monitor->write_filename );
			}
		}

		free( chan->monitor->format );
		free( chan->monitor );
		chan->monitor = NULL;
	}

	if( need_lock ) {
		ast_mutex_unlock(&chan->lock);
	}
	return 0;
}

/* Change monitoring filename of a channel */
int ast_monitor_change_fname(	struct ast_channel *chan,
								const char *fname_base, int need_lock )
{
	if( (!fname_base) || (!strlen(fname_base)) ) {
		ast_log(	LOG_WARNING,
					"Cannot change monitor filename of channel %s to null",
					chan->name );
		return -1;
	}
	
	if( need_lock ) {
		if (ast_mutex_lock(&chan->lock)) {
			ast_log(LOG_WARNING, "Unable to lock channel\n");
			return -1;
		}
	}

	if( chan->monitor ) {
		snprintf(	chan->monitor->filename_base, FILENAME_MAX, "%s/%s",
					AST_MONITOR_DIR, fname_base );
	} else {
		ast_log(	LOG_WARNING,
					"Cannot change monitor filename of channel %s to %s, monitoring not started",
					chan->name, fname_base );
				
	}

	if( need_lock ) {
		ast_mutex_unlock(&chan->lock);
	}
	return 0;
}

static int start_monitor_exec(struct ast_channel *chan, void *data)
{
	char *arg = NULL;
	char *format = NULL;
	char *fname_base = NULL;
	int res;
	
	/* Parse arguments. */
	if( data && strlen((char*)data) ) {
		arg = strdup( (char*)data );
		format = arg;
		fname_base = strchr( arg, '|' );
		if( fname_base ) {
			*fname_base = 0;
			fname_base++;
		}
	}

	res = ast_monitor_start( chan, format, fname_base, 1 );
	if( res < 0 ) {
		res = ast_monitor_change_fname( chan, fname_base, 1 );
	}

	if( arg ) {
		free( arg );
	}

	return res;
}

static int stop_monitor_exec(struct ast_channel *chan, void *data)
{
	return ast_monitor_stop( chan, 1 );
}

static int change_monitor_exec(struct ast_channel *chan, void *data)
{
	return ast_monitor_change_fname( chan, (const char*)data, 1 );
}

static int start_monitor_action(struct mansession *s, struct message *m)
{
	struct ast_channel *c = NULL;
	char *name = astman_get_header(m, "Channel");
	char *fname = astman_get_header(m, "File");
	char *format = astman_get_header(m, "Format");
	if((!name)||(!strlen(name))) {
		astman_send_error(s, m, "No channel specified");
		return 0;
	}
	c = ast_channel_walk(NULL);
	while(c) {
		if (!strcasecmp(c->name, name)) {
			break;
		}
		c = ast_channel_walk(c);
	}
	if (!c) {
		astman_send_error(s, m, "No such channel");
		return 0;
	}
	if( ast_monitor_start( c, format, fname, 1 ) ) {
		if( ast_monitor_change_fname( c, fname, 1 ) ) {
			astman_send_error(s, m, "Could not start monitoring channel");
			return 0;
		}
	}
	astman_send_ack(s, m, "Started monitoring channel");
	return 0;
}

static int stop_monitor_action(struct mansession *s, struct message *m)
{
	struct ast_channel *c = NULL;
	char *name = astman_get_header(m, "Channel");
	if((!name)||(!strlen(name))) {
		astman_send_error(s, m, "No channel specified");
		return 0;
	}
	c = ast_channel_walk(NULL);
	while(c) {
		if (!strcasecmp(c->name, name)) {
			break;
		}
		c = ast_channel_walk(c);
	}
	if (!c) {
		astman_send_error(s, m, "No such channel");
		return 0;
	}
	if( ast_monitor_stop( c, 1 ) ) {
		astman_send_error(s, m, "Could not stop monitoring channel");
		return 0;
	}
	astman_send_ack(s, m, "Stopped monitoring channel");
	return 0;
}

static int change_monitor_action(struct mansession *s, struct message *m)
{
	struct ast_channel *c = NULL;
	char *name = astman_get_header(m, "Channel");
	char *fname = astman_get_header(m, "File");
	if((!name) || (!strlen(name))) {
		astman_send_error(s, m, "No channel specified");
		return 0;
	}
	if ((!fname)||(!strlen(fname))) {
		astman_send_error(s, m, "No filename specified");
		return 0;
	}
	c = ast_channel_walk(NULL);
	while(c) {
		if (!strcasecmp(c->name, name)) {
			break;
		}
		c = ast_channel_walk(c);
	}
	if (!c) {
		astman_send_error(s, m, "No such channel");
		return 0;
	}
	if( ast_monitor_change_fname( c, fname, 1 ) ) {
		astman_send_error(s, m, "Could not change monitored filename of channel");
		return 0;
	}
	astman_send_ack(s, m, "Stopped monitoring channel");
	return 0;
}

int load_module(void)
{
	ast_register_application( "Monitor", start_monitor_exec, monitor_synopsis, monitor_descrip );
	ast_register_application( "StopMonitor", stop_monitor_exec, stopmonitor_synopsis, stopmonitor_descrip );
	ast_register_application( "ChangeMonitor", change_monitor_exec, changemonitor_synopsis, changemonitor_descrip );
	ast_manager_register( "Monitor", EVENT_FLAG_CALL, start_monitor_action, monitor_synopsis );
	ast_manager_register( "StopMonitor", EVENT_FLAG_CALL, stop_monitor_action, stopmonitor_synopsis );
	ast_manager_register( "ChangeMonitor", EVENT_FLAG_CALL, change_monitor_action, changemonitor_synopsis );

	return 0;
}

int unload_module(void)
{
	ast_unregister_application( "Monitor" );
	ast_unregister_application( "StopMonitor" );
	return 0;
}

char *description(void)
{
	return "Call Monitoring Resource";
}

int usecount(void)
{
	/* Never allow monitor to be unloaded because it will
	   unresolve needed symbols in the channel */
#if 0
	int res;
	STANDARD_USECOUNT(res);
	return res;
#else
	return 1;
#endif
}

char *key()
{
	return ASTERISK_GPL_KEY;
}
