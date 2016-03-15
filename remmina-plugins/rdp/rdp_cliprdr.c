/*
 * Remmina - The GTK+ Remote Desktop Client
 * Copyright (C) 2012-2012 Jean-Louis Dupond
 * Copyright (C) 2014-2015 Antenore Gatta, Fabio Castelli, Giovanni Panozzo
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 *
 *  In addition, as a special exception, the copyright holders give
 *  permission to link the code of portions of this program with the
 *  OpenSSL library under certain conditions as described in each
 *  individual source file, and distribute linked combinations
 *  including the two.
 *  You must obey the GNU General Public License in all respects
 *  for all of the code used other than OpenSSL. *  If you modify
 *  file(s) with this exception, you may extend this exception to your
 *  version of the file(s), but you are not obligated to do so. *  If you
 *  do not wish to do so, delete this exception statement from your
 *  version. *  If you delete this exception statement from all source
 *  files in the program, then also delete it here.
 *
 */

#include "rdp_plugin.h"
#include "rdp_cliprdr.h"
#include "rdp_event.h"

#include <freerdp/freerdp.h>
#include <freerdp/channels/channels.h>
#include <freerdp/client/cliprdr.h>
#include <sys/time.h>

#define CLIPBOARD_TRANSFER_WAIT_TIME 2

#undef TRACE_CALL
#define TRACE_CALL(x) printf("%s\n",x)

/* For file transfer, see https://github.com/FreeRDP/FreeRDP/blob/master/client/Windows/wf_cliprdr.c */

/* Windows shell clipboard formats
 * are registered locally as CF_LOCAL_FILEDESCRIPTOR CF_LOCAL_FILECONTENTS */
#define CFSTR_FILEDESCRIPTORW "FileGroupDescriptorW"
#define CFSTR_FILECONTENTS "FileContents"

/* Some random local cliboard formats id */
#define CF_LOCAL_FILEDESCRIPTOR 49288
#define CF_LOCAL_FILECONTENTS 49290

#define FD_ATTRIBUTES 0x04
#define FD_WRITESTIME 0x20
#define FD_FILESIZE 0x40
#define FD_PROGRESSUI 0x4000
const UINT64 EPOCH_DIFF = 11644473600ULL;

#define WIN_MAX_PATH 0x0104


typedef struct
{
    LONG        cx;
    LONG        cy;
} WIN_SIZEL;

typedef struct {
    ULONG  Data1;
    USHORT Data2;
    USHORT Data3;
    BYTE  Data4[ 8 ];
} WIN_CLSID;

typedef struct {
    LONG  x;
    LONG  y;
} WIN_POINTL;

typedef struct _FILEDESCRIPTORW {
	DWORD dwFlags;

	WIN_CLSID clsid;
	WIN_SIZEL sizel;
	WIN_POINTL pointl;

	DWORD dwFileAttributes;
	FILETIME ftCreationTime;
	FILETIME ftLastAccessTime;
	FILETIME ftLastWriteTime;
	DWORD nFileSizeHigh;
	DWORD nFileSizeLow;
	WCHAR  cFileName[ WIN_MAX_PATH ];
} FILEDESCRIPTORW;

typedef struct _FILEGROUPDESCRIPTORW {
	UINT cItems;
	FILEDESCRIPTORW fgd[1];
} FILEGROUPDESCRIPTORW;

struct rf_clipboard
{
	rfContext* rfi;
	CliprdrClientContext* context;
	wClipboard* system;
	int requestedFormatId;

	gboolean clipboard_busy;
	UINT32 format;

	pthread_mutex_t transfer_clip_mutex;
	pthread_cond_t transfer_clip_cond;
	enum  { SCDW_NONE, SCDW_BUSY_WAIT, SCDW_ASYNCWAIT } srv_clip_data_wait ;
	gpointer srv_data;

	/* For file transfer request */
	CLIPRDR_FILE_CONTENTS_REQUEST* lastFileContentsRequest;

	size_t nFiles;
	size_t file_array_size;
	char** file_names;
	FILEDESCRIPTORW** fileDescriptor;
};
typedef struct rf_clipboard rfClipboard;

UINT32 remmina_rdp_cliprdr_get_format_from_gdkatom(GdkAtom atom)
{
	TRACE_CALL("remmina_rdp_cliprdr_get_format_from_gdkatom");
	UINT32 rc;
	gchar* name = gdk_atom_name(atom);
	rc = 0;
	if (g_strcmp0("UTF8_STRING", name) == 0 || g_strcmp0("text/plain;charset=utf-8", name) == 0)
	{
		rc = CF_UNICODETEXT;
	}
	if (g_strcmp0("TEXT", name) == 0 || g_strcmp0("text/plain", name) == 0)
	{
		rc =  CF_TEXT;
	}
	if (g_strcmp0("text/html", name) == 0)
	{
		rc =  CB_FORMAT_HTML;
	}
	if (g_strcmp0("image/png", name) == 0)
	{
		rc =  CB_FORMAT_PNG;
	}
	if (g_strcmp0("image/jpeg", name) == 0)
	{
		rc =  CB_FORMAT_JPEG;
	}
	if (g_strcmp0("image/bmp", name) == 0)
	{
		rc =  CF_DIB;
	}
	g_free(name);
	return rc;
}

void remmina_rdp_cliprdr_get_target_types(UINT32** formats, UINT16* size, GdkAtom* types, int count)
{
	TRACE_CALL("remmina_rdp_cliprdr_get_target_types");
	int i;
	*size = 1;
	*formats = (UINT32*) malloc(sizeof(UINT32) * (count+1));

	*formats[0] = 0;
	for (i = 0; i < count; i++)
	{
		UINT32 format = remmina_rdp_cliprdr_get_format_from_gdkatom(types[i]);
		if (format != 0)
		{
			(*formats)[*size] = format;
			(*size)++;
		}
	}

	*formats = realloc(*formats, sizeof(UINT32) * (*size));
}

static UINT8* lf2crlf(UINT8* data, int* size)
{
	TRACE_CALL("lf2crlf");
	UINT8 c;
	UINT8* outbuf;
	UINT8* out;
	UINT8* in_end;
	UINT8* in;
	int out_size;

	out_size = (*size) * 2 + 1;
	outbuf = (UINT8*) malloc(out_size);
	out = outbuf;
	in = data;
	in_end = data + (*size);

	while (in < in_end)
	{
		c = *in++;
		if (c == '\n')
		{
			*out++ = '\r';
			*out++ = '\n';
		}
		else
		{
			*out++ = c;
		}
	}

	*out++ = 0;
	*size = out - outbuf;

	return outbuf;
}

static void crlf2lf(UINT8* data, size_t* size)
{
	TRACE_CALL("crlf2lf");
	UINT8 c;
	UINT8* out;
	UINT8* in;
	UINT8* in_end;

	out = data;
	in = data;
	in_end = data + (*size);

	while (in < in_end)
	{
		c = *in++;
		if (c != '\r')
			*out++ = c;
	}

	*size = out - data;
}

static UINT cliprdr_send_response_filecontents(rfClipboard* clipboard,
											   UINT32 streamId, UINT32 size,
											   BYTE* data)
{
	TRACE_CALL("cliprdr_send_response_filecontents");

	CLIPRDR_FILE_CONTENTS_RESPONSE fileContentsResponse;

	if (!clipboard || !clipboard->context || !clipboard->context->ClientFileContentsResponse)
		return ERROR_INTERNAL_ERROR;

	fileContentsResponse.streamId = streamId;
	fileContentsResponse.cbRequested = size;
	fileContentsResponse.requestedData = data;
	fileContentsResponse.msgFlags = CB_RESPONSE_OK;

	printf("GIO: sending filecontents response streamId=%d data=%p size=%u\n", streamId, (void *)data, size);
	return clipboard->context->ClientFileContentsResponse(clipboard->context, &fileContentsResponse);
}


static BOOL remmina_cliprdr_get_file_contents(char* file_name, BYTE* buffer,
										 LONG positionLow, LONG positionHigh,
										 DWORD nRequested, DWORD* puSize)
{
	int fd;
	off_t off, sk;
	ssize_t nread;

	if (!file_name || !buffer || !puSize)
		return FALSE;

	fd = open(file_name, O_RDONLY);
	if (fd == -1)
		return FALSE;

	off = (off_t)positionLow + ((off_t)positionHigh << 32); /* ToDo: support positionHigh on 32bit systems */
	sk = lseek(fd, off, SEEK_SET);

	if (sk != off) {
		close(fd);
		return FALSE;
	}

	nread = read(fd, buffer, nRequested);

	if (nread == -1) {
		/* ToDo: support EINTR ? Log error message */
		close(fd);
		return FALSE;
	}

	printf("GIO: file is %s off=%u nRequested=%u nread=%d\n", file_name, (unsigned)off, (unsigned)nRequested, (int)nread);

	close(fd);
	*puSize = (DWORD)nread;

	return TRUE;


}


static void remmina_rdp_cliprdr_mt_server_file_contents_request(RemminaProtocolWidget* gp, RemminaPluginRdpUiObject* ui)
{
	TRACE_CALL("remmina_rdp_cliprdr_mt_server_file_contents_request");

	printf("GIO: remmina_rdp_cliprdr_mt_server_file_contents_request\n");

	/* We are on the main thread. Subthread is doing other operations and not waiting for us.
	 * We just ask the Gtk Clipboard to enumerate the URIS it have */

	rfContext* rfi;
	DWORD uSize = 0;
	BYTE* pData = NULL;
	int sRc;
	char *fileName;
	CLIPRDR_FILE_CONTENTS_REQUEST* fileContentsRequest;

	rfi = GET_PLUGIN_DATA(gp);
	if (!rfi || !rfi->clipboard || !rfi->clipboard->lastFileContentsRequest)
		return;

	fileContentsRequest = rfi->clipboard->lastFileContentsRequest;
	printf("GIO: have a request for file listIndex=%d streamid=%d\n",fileContentsRequest->listIndex, fileContentsRequest->streamId);


	if (fileContentsRequest->dwFlags == FILECONTENTS_SIZE) {
		printf("GIO: request is for the size of the file\n");
		fileContentsRequest->cbRequested = sizeof(UINT64);
	}

	pData = (BYTE*) calloc(1, fileContentsRequest->cbRequested);
	if (!pData)
		goto error;

	fileName = rfi->clipboard->file_names[fileContentsRequest->listIndex];
	if (fileContentsRequest->dwFlags == FILECONTENTS_SIZE) {
		*((UINT32*) &pData[0]) = rfi->clipboard->fileDescriptor[fileContentsRequest->listIndex]->nFileSizeLow;
		*((UINT32*) &pData[4]) = rfi->clipboard->fileDescriptor[fileContentsRequest->listIndex]->nFileSizeHigh;
		uSize = fileContentsRequest->cbRequested;
	} else {
		BOOL bRet;
		bRet = remmina_cliprdr_get_file_contents(rfi->clipboard->file_names[fileContentsRequest->listIndex], pData,
			fileContentsRequest->nPositionLow, fileContentsRequest->nPositionHigh, fileContentsRequest->cbRequested, &uSize);
		if (bRet == FALSE) {
			uSize = 0;
			goto error;
		}
	}


error:
	sRc = cliprdr_send_response_filecontents(rfi->clipboard, fileContentsRequest->streamId, uSize, pData);

	free(pData);

	free(fileContentsRequest);
	rfi->clipboard->lastFileContentsRequest = NULL;

}

UINT remmina_rdp_cliprdr_server_file_contents_request(CliprdrClientContext* context, CLIPRDR_FILE_CONTENTS_REQUEST* fileContentsRequest)
{
	TRACE_CALL("remmina_rdp_cliprdr_server_file_contents_request");

	/* Directly called by freerdp subthread when the user pastes a file on the remote desktop */

	rfClipboard* clipboard = (rfClipboard*)context->custom;
	RemminaProtocolWidget* gp;
	rfContext* rfi;
	RemminaPluginRdpUiObject *ui;

	if (!context || !fileContentsRequest)
		return ERROR_INTERNAL_ERROR;

	clipboard = (rfClipboard*)context->custom;
	if (!clipboard)
		return ERROR_INTERNAL_ERROR;

	rfi = clipboard->rfi;
	if (!rfi)
		return ERROR_INTERNAL_ERROR;

	if (rfi->clipboard != clipboard) {
		remmina_plugin_service->log_printf("[RDP] Internal error in %s: clipboard received from libfreerdp is not the same as the one in our rfContext\n", __FUNCTION__);
		return ERROR_INTERNAL_ERROR;
	}

	if (clipboard->lastFileContentsRequest) {
		remmina_plugin_service->log_printf("[RDP] there is already an outstanting file contents request for this clipboad/connection. Cannot serve a second one.\n");
		return ERROR_INTERNAL_ERROR;
	}

	clipboard->lastFileContentsRequest = malloc(sizeof(CLIPRDR_FILE_CONTENTS_REQUEST));
	if (!clipboard->lastFileContentsRequest)
		return ERROR_INTERNAL_ERROR;

	*(clipboard->lastFileContentsRequest) = *(fileContentsRequest);

	gp = clipboard->rfi->protocol_widget;

	ui = g_new0(RemminaPluginRdpUiObject, 1);
	ui->type = REMMINA_RDP_UI_CLIPBOARD;
	ui->clipboard.type = REMMINA_RDP_UI_CLIPBOARD_FILE_CONTENTS_REQUEST;
	ui->sync = FALSE;
	remmina_rdp_event_queue_ui(gp, ui);

	return CHANNEL_RC_OK;
}


UINT remmina_rdp_cliprdr_server_file_contents_response(CliprdrClientContext* context, CLIPRDR_FILE_CONTENTS_RESPONSE* fileContentsResponse)
{
	TRACE_CALL("remmina_rdp_cliprdr_server_file_contents_response");
/*	rfClipboard* clipboard;

	if (fileContentsResponse->msgFlags != CB_RESPONSE_OK)
		return E_FAIL;

	if (!context || !fileContentsResponse)
		return ERROR_INTERNAL_ERROR;

	clipboard = (rfClipboard*) context->custom;

	if (!clipboard)
		return ERROR_INTERNAL_ERROR;

	clipboard->req_fsize = fileContentsResponse->cbRequested;
	clipboard->req_fdata = (char*) malloc(fileContentsResponse->cbRequested);
	if (!clipboard->req_fdata)
		return ERROR_INTERNAL_ERROR;

	CopyMemory(clipboard->req_fdata, fileContentsResponse->requestedData, fileContentsResponse->cbRequested);

	if (!SetEvent(clipboard->req_fevent))
	{
		free (clipboard->req_fdata);
		return ERROR_INTERNAL_ERROR;
	}
*/
	return CHANNEL_RC_OK;
	return 1;
}


static UINT remmina_rdp_cliprdr_monitor_ready(CliprdrClientContext* context, CLIPRDR_MONITOR_READY* monitorReady)
{
	TRACE_CALL("remmina_rdp_cliprdr_monitor_ready");
	RemminaPluginRdpUiObject* ui;
	rfClipboard* clipboard = (rfClipboard*)context->custom;
	RemminaProtocolWidget* gp;

	gp = clipboard->rfi->protocol_widget;

	ui = g_new0(RemminaPluginRdpUiObject, 1);
	ui->type = REMMINA_RDP_UI_CLIPBOARD;
	ui->clipboard.type = REMMINA_RDP_UI_CLIPBOARD_MONITORREADY;
	ui->sync = TRUE;
	remmina_rdp_event_queue_ui(gp, ui);

	return CHANNEL_RC_OK;
}

static UINT remmina_rdp_cliprdr_server_capabilities(CliprdrClientContext* context, CLIPRDR_CAPABILITIES* capabilities)
{
	TRACE_CALL("remmina_rdp_cliprdr_server_capabilities");
	return CHANNEL_RC_OK;
}


static UINT remmina_rdp_cliprdr_server_format_list(CliprdrClientContext* context, CLIPRDR_FORMAT_LIST* formatList)
{
	TRACE_CALL("remmina_rdp_cliprdr_server_format_list");

	/* Called when a user do a "Copy" on the server: we collect all formats
	 * the server send us and then setup the local clipboard with the appropiate
	 * functions to request server data */

	RemminaPluginRdpUiObject* ui;
	RemminaProtocolWidget* gp;
	rfClipboard* clipboard;
	CLIPRDR_FORMAT* format;

	int i;

	clipboard = (rfClipboard*)context->custom;
	gp = clipboard->rfi->protocol_widget;
	GtkTargetList* list = gtk_target_list_new (NULL, 0);

	for (i = 0; i < formatList->numFormats; i++)
	{
		format = &formatList->formats[i];
		if (format->formatId == CF_UNICODETEXT)
		{
			GdkAtom atom = gdk_atom_intern("UTF8_STRING", TRUE);
			gtk_target_list_add(list, atom, 0, CF_UNICODETEXT);
		}
		else if (format->formatId == CF_TEXT)
		{
			GdkAtom atom = gdk_atom_intern("TEXT", TRUE);
			gtk_target_list_add(list, atom, 0, CF_TEXT);
		}
		else if (format->formatId == CF_DIB)
		{
			GdkAtom atom = gdk_atom_intern("image/bmp", TRUE);
			gtk_target_list_add(list, atom, 0, CF_DIB);
		}
		else if (format->formatId == CF_DIBV5)
		{
			GdkAtom atom = gdk_atom_intern("image/bmp", TRUE);
			gtk_target_list_add(list, atom, 0, CF_DIBV5);
		}
		else if (format->formatId == CB_FORMAT_JPEG)
		{
			GdkAtom atom = gdk_atom_intern("image/jpeg", TRUE);
			gtk_target_list_add(list, atom, 0, CB_FORMAT_JPEG);
		}
		else if (format->formatId == CB_FORMAT_PNG)
		{
			GdkAtom atom = gdk_atom_intern("image/png", TRUE);
			gtk_target_list_add(list, atom, 0, CB_FORMAT_PNG);
		}
		else if (format->formatId == CB_FORMAT_HTML)
		{
			GdkAtom atom = gdk_atom_intern("text/html", TRUE);
			gtk_target_list_add(list, atom, 0, CB_FORMAT_HTML);
		}
	}

	ui = g_new0(RemminaPluginRdpUiObject, 1);
	ui->type = REMMINA_RDP_UI_CLIPBOARD;
	ui->clipboard.type = REMMINA_RDP_UI_CLIPBOARD_SET_DATA;
	ui->clipboard.targetlist = list;
	ui->sync = TRUE;
	remmina_rdp_event_queue_ui(gp, ui);

	return CHANNEL_RC_OK;
}

static UINT remmina_rdp_cliprdr_server_format_list_response(CliprdrClientContext* context, CLIPRDR_FORMAT_LIST_RESPONSE* formatListResponse)
{
	TRACE_CALL("remmina_rdp_cliprdr_server_format_list_response");

	if (formatListResponse->msgFlags != CB_RESPONSE_OK)
		return E_FAIL;
	return CHANNEL_RC_OK;
}


static UINT remmina_rdp_cliprdr_server_format_data_request(CliprdrClientContext* context, CLIPRDR_FORMAT_DATA_REQUEST* formatDataRequest)
{
	TRACE_CALL("remmina_rdp_cliprdr_server_format_data_request");

	RemminaPluginRdpUiObject* ui;
	RemminaProtocolWidget* gp;
	rfClipboard* clipboard;

	clipboard = (rfClipboard*)context->custom;
	gp = clipboard->rfi->protocol_widget;

	ui = g_new0(RemminaPluginRdpUiObject, 1);
	ui->type = REMMINA_RDP_UI_CLIPBOARD;
	ui->clipboard.type = REMMINA_RDP_UI_CLIPBOARD_SERVER_FORMAT_DATA_REQUEST;
	ui->clipboard.format = formatDataRequest->requestedFormatId;
	ui->sync = TRUE;
	remmina_rdp_event_queue_ui(gp, ui);

	return CHANNEL_RC_OK;
}

static UINT remmina_rdp_cliprdr_server_format_data_response(CliprdrClientContext* context, CLIPRDR_FORMAT_DATA_RESPONSE* formatDataResponse)
{
	TRACE_CALL("remmina_rdp_cliprdr_server_format_data_response");

	UINT8* data;
	size_t size;
	rfContext* rfi;
	RemminaProtocolWidget* gp;
	rfClipboard* clipboard;
	GdkPixbufLoader *pixbuf;
	gpointer output = NULL;
	RemminaPluginRdpUiObject *ui;

	clipboard = (rfClipboard*)context->custom;
	gp = clipboard->rfi->protocol_widget;
	rfi = GET_PLUGIN_DATA(gp);

	data = formatDataResponse->requestedFormatData;
	size = formatDataResponse->dataLen;

	// formatDataResponse->requestedFormatData is allocated
	//  by freerdp and freed after returning from this callback function.
	//  So we must make a copy if we need to preserve it

	if (size > 0)
	{
		switch (rfi->clipboard->format)
		{
			case CF_UNICODETEXT:
			{
				size = ConvertFromUnicode(CP_UTF8, 0, (WCHAR*)data, size / 2, (CHAR**)&output, 0, NULL, NULL);
				crlf2lf(output, &size);
				break;
			}

			case CF_TEXT:
			case CB_FORMAT_HTML:
			{
				output = (gpointer)calloc(1, size + 1);
				if (output) {
					memcpy(output, data, size);
					crlf2lf(output, &size);
				}
				break;
			}

			case CF_DIBV5:
			case CF_DIB:
			{
				wStream* s;
				UINT32 offset;
				GError *perr;
				BITMAPINFOHEADER* pbi;
				BITMAPV5HEADER* pbi5;

				pbi = (BITMAPINFOHEADER*)data;

				// offset calculation inspired by http://downloads.poolelan.com/MSDN/MSDNLibrary6/Disk1/Samples/VC/OS/WindowsXP/GetImage/BitmapUtil.cpp
				offset = 14 + pbi->biSize;
				if (pbi->biClrUsed != 0)
					offset += sizeof(RGBQUAD) * pbi->biClrUsed;
				else if (pbi->biBitCount <= 8)
					offset += sizeof(RGBQUAD) * (1 << pbi->biBitCount);
				if (pbi->biSize == sizeof(BITMAPINFOHEADER)) {
					if (pbi->biCompression == 3) // BI_BITFIELDS is 3
							offset += 12;
				} else if (pbi->biSize >= sizeof(BITMAPV5HEADER)) {
					pbi5 = (BITMAPV5HEADER*)pbi;
					if (pbi5->bV5ProfileData <= offset)
							offset += pbi5->bV5ProfileSize;
				}
				s = Stream_New(NULL, 14 + size);
				Stream_Write_UINT8(s, 'B');
				Stream_Write_UINT8(s, 'M');
				Stream_Write_UINT32(s, 14 + size);
				Stream_Write_UINT32(s, 0);
				Stream_Write_UINT32(s, offset);
				Stream_Write(s, data, size);

				data = Stream_Buffer(s);
				size = Stream_Length(s);

				pixbuf = gdk_pixbuf_loader_new();
				perr = NULL;
				if ( !gdk_pixbuf_loader_write(pixbuf, data, size, &perr) ) {
						remmina_plugin_service->log_printf("[RDP] rdp_cliprdr: gdk_pixbuf_loader_write() returned error %s\n", perr->message);
				}
				else
				{
					if ( !gdk_pixbuf_loader_close(pixbuf, &perr) ) {
						remmina_plugin_service->log_printf("[RDP] rdp_cliprdr: gdk_pixbuf_loader_close() returned error %s\n", perr->message);
						perr = NULL;
					}
					Stream_Free(s, TRUE);
					output = g_object_ref(gdk_pixbuf_loader_get_pixbuf(pixbuf));
				}
				g_object_unref(pixbuf);
				break;
			}

			case CB_FORMAT_PNG:
			case CB_FORMAT_JPEG:
			{
				pixbuf = gdk_pixbuf_loader_new();
				gdk_pixbuf_loader_write(pixbuf, data, size, NULL);
				output = g_object_ref(gdk_pixbuf_loader_get_pixbuf(pixbuf));
				gdk_pixbuf_loader_close(pixbuf, NULL);
				g_object_unref(pixbuf);
				break;
			}
		}
	}

	pthread_mutex_lock(&clipboard->transfer_clip_mutex);
	pthread_cond_signal(&clipboard->transfer_clip_cond);
	if ( clipboard->srv_clip_data_wait == SCDW_BUSY_WAIT ) {
		clipboard->srv_data = output;
	}
	else
	{
		// Clipboard data arrived from server when we are not busywaiting.
		// Just put it on the local clipboard

		ui = g_new0(RemminaPluginRdpUiObject, 1);
		ui->type = REMMINA_RDP_UI_CLIPBOARD;
		ui->clipboard.type = REMMINA_RDP_UI_CLIPBOARD_SET_CONTENT;
		ui->clipboard.data = output;
		ui->clipboard.format = clipboard->format;
		remmina_rdp_event_queue_ui(gp, ui);

		clipboard->srv_clip_data_wait = SCDW_NONE;

	}
	pthread_mutex_unlock(&clipboard->transfer_clip_mutex);

	return CHANNEL_RC_OK;
}

void remmina_rdp_cliprdr_request_data(GtkClipboard *gtkClipboard, GtkSelectionData *selection_data, guint info, RemminaProtocolWidget* gp )
{
	TRACE_CALL("remmina_rdp_cliprdr_request_data");
	/* Called when someone press "Paste" on the client side.
	 * We ask to the server the data we need */

	GdkAtom target;
	CLIPRDR_FORMAT_DATA_REQUEST request;
	rfClipboard* clipboard;
	rfContext* rfi = GET_PLUGIN_DATA(gp);
	struct timespec to;
	struct timeval tv;
	int rc;

	clipboard = rfi->clipboard;
	if ( clipboard->srv_clip_data_wait != SCDW_NONE ) {
		remmina_plugin_service->log_printf("[RDP] Cannot paste now, I'm transferring clipboard data from server. Try again later\n");
		return;
	}

	target = gtk_selection_data_get_target(selection_data);
	// clipboard->format = remmina_rdp_cliprdr_get_format_from_gdkatom(target);
	clipboard->format = info;

	/* Request Clipboard content from the server */
	ZeroMemory(&request, sizeof(CLIPRDR_FORMAT_DATA_REQUEST));
	request.requestedFormatId = clipboard->format;
	request.msgFlags = CB_RESPONSE_OK;
	request.msgType = CB_FORMAT_DATA_REQUEST;


	pthread_mutex_lock(&clipboard->transfer_clip_mutex);

	clipboard->srv_clip_data_wait = SCDW_BUSY_WAIT;
	clipboard->context->ClientFormatDataRequest(clipboard->context, &request);

	/* Busy wait clibpoard data for CLIPBOARD_TRANSFER_WAIT_TIME seconds */
	gettimeofday(&tv, NULL);
	to.tv_sec = tv.tv_sec + CLIPBOARD_TRANSFER_WAIT_TIME;
	to.tv_nsec = tv.tv_usec * 1000;
	rc = pthread_cond_timedwait(&clipboard->transfer_clip_cond,&clipboard->transfer_clip_mutex, &to);

	if ( rc == 0 ) {
		/* Data has arrived without timeout */
		if (clipboard->srv_data != NULL)
		{
			if (info == CB_FORMAT_PNG || info == CF_DIB || info == CF_DIBV5 || info == CB_FORMAT_JPEG)
			{
				gtk_selection_data_set_pixbuf(selection_data, clipboard->srv_data);
				g_object_unref(clipboard->srv_data);
			}
			else
			{
				gtk_selection_data_set_text(selection_data, clipboard->srv_data, -1);
				free(clipboard->srv_data);
			}
		}
		clipboard->srv_clip_data_wait = SCDW_NONE;
	} else {
		clipboard->srv_clip_data_wait = SCDW_ASYNCWAIT;
		if ( rc == ETIMEDOUT ) {
			remmina_plugin_service->log_printf("[RDP] Clipboard data has not been transfered from the server in %d seconds. Try to paste later.\n",
				CLIPBOARD_TRANSFER_WAIT_TIME);
		}
		else {
			remmina_plugin_service->log_printf("[RDP] internal error: pthread_cond_timedwait() returned %d\n",rc);
			clipboard->srv_clip_data_wait = SCDW_NONE;
		}
	}
	pthread_mutex_unlock(&clipboard->transfer_clip_mutex);

}

void remmina_rdp_cliprdr_empty_clipboard(GtkClipboard *gtkClipboard, rfClipboard *clipboard)
{
	TRACE_CALL("remmina_rdp_cliprdr_empty_clipboard");
	/* No need to do anything here */
}

static UINT remmina_rdp_cliprdr_send_client_capabilities(rfClipboard* clipboard)
{
	TRACE_CALL("remmina_rdp_cliprdr_send_client_capabilities");
	CLIPRDR_CAPABILITIES capabilities;
	CLIPRDR_GENERAL_CAPABILITY_SET generalCapabilitySet;

	if (!clipboard || !clipboard->context || !clipboard->context->ClientCapabilities)
		return ERROR_INTERNAL_ERROR;

	capabilities.cCapabilitiesSets = 1;
	capabilities.capabilitySets = (CLIPRDR_CAPABILITY_SET*) &(generalCapabilitySet);

	generalCapabilitySet.capabilitySetType = CB_CAPSTYPE_GENERAL;
	generalCapabilitySet.capabilitySetLength = 12;

	generalCapabilitySet.version = CB_CAPS_VERSION_2;
	generalCapabilitySet.generalFlags = CB_USE_LONG_FORMAT_NAMES | CB_STREAM_FILECLIP_ENABLED | CB_FILECLIP_NO_FILE_PATHS;

	return clipboard->context->ClientCapabilities(clipboard->context, &capabilities);

}

static gboolean remmina_rdp_cliprdr_is_valid_file_or_dir_uri(char *uri)
{
	struct stat sb;
	char *unescaped_uri;

	if (strncmp(uri, "file://", 7) != 0)
		return FALSE;

	unescaped_uri = g_uri_unescape_string(uri, NULL);

	if (lstat(unescaped_uri+7, &sb) != 0) {
		g_free(unescaped_uri);
		return FALSE;
	}

	g_free(unescaped_uri);

	if (!S_ISREG(sb.st_mode) && !S_ISDIR(sb.st_mode))
		return FALSE;

	printf("GIO: %s is a valid file URI\n", uri);

	return TRUE;
}

int remmina_rdp_cliprdr_mt_send_format_list(RemminaProtocolWidget* gp, RemminaPluginRdpUiObject* ui)
{
	TRACE_CALL("remmina_rdp_cliprdr_mt_send_format_list");
	GtkClipboard* gtkClipboard;
	rfClipboard* clipboard;
	rfContext* rfi = GET_PLUGIN_DATA(gp);
	GdkAtom* targets;
	gboolean result = 0, hasfiles;
	gint loccount, srvcount;
	gint formatId, i;
	CLIPRDR_FORMAT_LIST formatList;
	CLIPRDR_FORMAT* formats;
	CLIPRDR_FORMAT* formats_new;
	gchar **uris, **urip;

	clipboard = rfi->clipboard;
	if (!clipboard)
		return 0;

	formatList.formats = formats = NULL;
	formatList.numFormats = 0;

	gtkClipboard = gtk_widget_get_clipboard(rfi->drawing_area, GDK_SELECTION_CLIPBOARD);
	if (gtkClipboard) {
		result = gtk_clipboard_wait_for_targets(gtkClipboard, &targets, &loccount);
	}

	if (result) {
		formats = (CLIPRDR_FORMAT*)malloc((loccount+1) * sizeof(CLIPRDR_FORMAT));
		srvcount = 0;

		/* Check if we have some file uris on the local clipboard */
		hasfiles = FALSE;
		uris = gtk_clipboard_wait_for_uris(gtkClipboard);
		if (uris) {
			urip = uris;
			while(*urip) {
				if (remmina_rdp_cliprdr_is_valid_file_or_dir_uri(*urip))
					hasfiles = TRUE;
				urip++;
			}
			g_strfreev(uris);
		}

		if (hasfiles) {
			printf("GIO: we have a file on the clipboard !\n");
			formats[srvcount].formatId = CF_LOCAL_FILEDESCRIPTOR;
			formats[srvcount].formatName = CFSTR_FILEDESCRIPTORW;
			srvcount++;
			formats[srvcount].formatId = CF_LOCAL_FILECONTENTS;
			formats[srvcount].formatName = CFSTR_FILECONTENTS;
			srvcount++;
		} else {
			for(i = 0 ; i < loccount ; i++)  {
				/* Standard non-file format list */
				formatId = remmina_rdp_cliprdr_get_format_from_gdkatom(targets[i]);
				if ( formatId != 0 ) {
					formats[srvcount].formatId = formatId;
					formats[srvcount].formatName = NULL;
					srvcount ++;
				}
			}
		}

		if (srvcount > 0) {
			formats_new = (CLIPRDR_FORMAT*)realloc(formats, srvcount * sizeof(CLIPRDR_FORMAT));
			if (formats_new == NULL) {
				printf("realloc failure in remmina_rdp_cliprdr_mt_send_format_list\n");
			} else {
				formats = formats_new;
			}
		} else {
			free(formats);
			formats = NULL;
		}
		g_free(targets);

		formatList.formats = formats;
		formatList.numFormats = srvcount;
	}

	formatList.msgFlags = CB_RESPONSE_OK;
	printf("GIO: sending %d formats to server\n", (int)formatList.numFormats);
	clipboard->context->ClientFormatList(clipboard->context, &formatList);

	if (formats)
		free(formats);

	return 1;
}


static void remmina_rdp_cliprdr_send_data_response(rfClipboard* clipboard, BYTE* data, int size)
{
	TRACE_CALL("remmina_rdp_cliprdr_send_data_response");
	CLIPRDR_FORMAT_DATA_RESPONSE response;

	ZeroMemory(&response, sizeof(CLIPRDR_FORMAT_DATA_RESPONSE));

	response.msgFlags = CB_RESPONSE_OK;
	response.dataLen = size;
	response.requestedFormatData = data;
	clipboard->context->ClientFormatDataResponse(clipboard->context, &response);
	free(data);
}


int remmina_rdp_cliprdr_mt_monitor_ready(RemminaProtocolWidget* gp, RemminaPluginRdpUiObject* ui)
{
	TRACE_CALL("remmina_rdp_cliprdr_mt_monitor_ready");
	rfContext* rfi;
	rfClipboard *clipboard;

	rfi = GET_PLUGIN_DATA(gp);
	if (!rfi)
		return 0;

	clipboard = rfi->clipboard;
	if (!clipboard)
		return 0;

	remmina_rdp_cliprdr_send_client_capabilities(clipboard);
	remmina_rdp_cliprdr_mt_send_format_list(gp,ui);

	return 1;
}

/* path_name has a '/' at the end. e.g. /home/user/newfolder/, file_name is /home/user/newfolder/new.txt */
static FILEDESCRIPTORW* remmina_cliprdr_get_file_descriptor(char* file_name, size_t pathLen)
{
	TRACE_CALL("remmina_cliprdr_get_file_descriptor");

	struct stat sb;
	FILEDESCRIPTORW* fd;
	UINT64 t;
	char *ufn;
	int cchFileName;

	fd = (FILEDESCRIPTORW*) calloc(1, sizeof(FILEDESCRIPTORW));

	if (!fd)
		return NULL;

	if (lstat(file_name, &sb) != 0) {
		free(fd);
		return NULL;
	}

	if (!S_ISREG(sb.st_mode) && !S_ISDIR(sb.st_mode)) {
		free(fd);
		return NULL;
	}

	fd->dwFlags = FD_ATTRIBUTES | FD_FILESIZE | FD_WRITESTIME | FD_PROGRESSUI;

	/* Convert UNIX timestamp in sb.st_mtime to windows FILETIME.
	 * Windows FILETIME has 100ns precision, starting from 1/1/1601  */

	t = (UINT64)sb.st_mtim.tv_nsec / 100ULL + 10000000ULL * (sb.st_mtim.tv_sec + EPOCH_DIFF);

	fd->ftLastWriteTime.dwLowDateTime = (DWORD)(t & 0xffffffffULL);
	fd->ftLastWriteTime.dwHighDateTime = (DWORD)(t >> 32);

	printf("GIO: file low time=%lu file high time=%lu\n", fd->ftLastWriteTime.dwLowDateTime, fd->ftLastWriteTime.dwHighDateTime);

	fd->nFileSizeLow = (DWORD)(sb.st_size & 0xffffffffULL);
	fd->nFileSizeHigh = (DWORD)(sb.st_size >> 32);

	fd->dwFileAttributes = 0;
	if (S_ISDIR(sb.st_mode))
		fd->dwFileAttributes |= FILE_ATTRIBUTE_DIRECTORY;

	/* The filename in file_name + pathLen must be converted to unicode
	 * and put to fd->cFileName */

	ufn = NULL;	// Asks ConvertToUnicode to allocate
	cchFileName = ConvertToUnicode(CP_UTF8, 0, file_name + pathLen, -1, (WCHAR**)&ufn, 0);
	if (cchFileName < 1 || cchFileName > ((sizeof(fd->cFileName)/sizeof(WCHAR))-1)) {
		free(fd);
		return NULL;
	}
	memcpy(fd->cFileName, ufn, cchFileName * sizeof(WCHAR));

	return fd;
}


static BOOL remmina_cliprdr_array_ensure_capacity(rfClipboard* clipboard)
{
	TRACE_CALL("remmina_cliprdr_array_ensure_capacity");

	if (!clipboard)
		return FALSE;

	if (clipboard->nFiles == clipboard->file_array_size)
	{
		size_t new_size;
		FILEDESCRIPTORW **new_fd;
		char **new_name;

		new_size = (clipboard->file_array_size + 1) * 2;

		new_fd = (FILEDESCRIPTORW**) realloc(clipboard->fileDescriptor,
											 new_size * sizeof(FILEDESCRIPTORW*));
		if (new_fd)
			clipboard->fileDescriptor = new_fd;

		new_name = (char**) realloc(clipboard->file_names, new_size * sizeof(char*));
		if (new_name)
			clipboard->file_names = new_name;

		if (!new_fd || !new_name)
			return FALSE;

		clipboard->file_array_size = new_size;
	}

	return TRUE;
}

static void clear_file_array(rfClipboard* clipboard)
{
	size_t i;
	if (!clipboard)
		return;
	if (clipboard->file_names) {
		for (i = 0; i < clipboard->nFiles; i++) {
			free(clipboard->file_names[i]);
			clipboard->file_names[i] = NULL;
		}
		free (clipboard->file_names);
		clipboard->file_names = NULL;
	}
	if (clipboard->fileDescriptor) {
		for (i = 0; i < clipboard->nFiles; i++) {
			free(clipboard->fileDescriptor[i]);
			clipboard->fileDescriptor[i] = NULL;
		}
		free (clipboard->fileDescriptor);
		clipboard->fileDescriptor = NULL;
	}
	clipboard->file_array_size = 0;
	clipboard->nFiles = 0;

}


static BOOL remmina_cliprdr_add_to_file_arrays(rfClipboard* clipboard, char* full_file_name, size_t pathLen)
{

	TRACE_CALL("remmina_cliprdr_add_to_file_arrays");

	size_t ffnlen;

	if (!remmina_cliprdr_array_ensure_capacity(clipboard))
		return FALSE;

	ffnlen = strlen(full_file_name);

	/* add to name array */
	clipboard->file_names[clipboard->nFiles] = (char *) malloc(ffnlen + 1);
	if (!clipboard->file_names[clipboard->nFiles])
		return FALSE;

	strncpy(clipboard->file_names[clipboard->nFiles], full_file_name, ffnlen+1);

	/* add to descriptor array */
	clipboard->fileDescriptor[clipboard->nFiles] = remmina_cliprdr_get_file_descriptor(full_file_name, pathLen);
	if (!clipboard->fileDescriptor[clipboard->nFiles])
	{
		free (clipboard->file_names[clipboard->nFiles]);
		return FALSE;
	}

	clipboard->nFiles++;

	return TRUE;
}

static BOOL remmina_cliprdr_traverse_directory(rfClipboard* clipboard, char *dir_name, size_t pathLen)
{
	DIR *dir;
	struct dirent de, *dep;
	struct stat sb;
	char *fullFileName;

	if (!clipboard || !dir_name)
		return FALSE;

	dir = opendir(dir_name);
	if (!dir)
		return FALSE;

	for(;;) {
		if(readdir_r(dir, &de, &dep) != 0) {
			return FALSE;
		}
		if (dep == NULL)
			break;

		if (strcmp(de.d_name,".") == 0 || strcmp(de.d_name,"..") == 0)
			continue;

		printf("GIO: file %s in dir %s\n", de.d_name, dir_name);

		fullFileName = malloc(strlen(dir_name)+strlen(de.d_name)+2);
		if (!fullFileName)
			return FALSE;

		strcpy(fullFileName, dir_name);
		strcat(fullFileName, "/");
		strcat(fullFileName, de.d_name);

		if (lstat(fullFileName, &sb) != 0) {
			free(fullFileName);
			return FALSE;
		}

		if (S_ISDIR(sb.st_mode)) {
			if (!remmina_cliprdr_add_to_file_arrays(clipboard, fullFileName, pathLen)) {
				free(fullFileName);
				return FALSE;
			}
			if (!remmina_cliprdr_traverse_directory(clipboard, fullFileName, pathLen)) {
				free(fullFileName);
				return FALSE;
			}
		} else if (S_ISREG(sb.st_mode)) {
			if (!remmina_cliprdr_add_to_file_arrays(clipboard, fullFileName, pathLen)) {
				free(fullFileName);
				return FALSE;
			}
		}

		free(fullFileName);

	}

	closedir(dir);

	return TRUE;
}

static BOOL remmina_cliprdr_process_filename(rfClipboard* clipboard, char *fileUri)
{

	TRACE_CALL("remmina_cliprdr_process_filename");

	size_t offset;
	size_t pathLen;
	char *fileName;

	if (!clipboard || !fileUri)
		return FALSE;

	if (strncmp(fileUri, "file://", 7) != 0)
		return FALSE;

	fileName = g_uri_unescape_string(fileUri + 7, NULL);
	offset = strlen(fileName);

	/* find the last '/' in full file name, should work with utf-8 filenames too */
	while(offset > 0) {
		if (fileName[offset] == '/')
			break;
		else
			offset--;
	}
	pathLen = offset + 1;
	if (!remmina_cliprdr_add_to_file_arrays(clipboard, fileName, pathLen)) {
		g_free(fileName);
		return FALSE;
	}
	if ((clipboard->fileDescriptor[clipboard->nFiles - 1]->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
		if (!remmina_cliprdr_traverse_directory(clipboard, fileName, pathLen)) {
			g_free(fileName);
			return FALSE;
		}
	}
	g_free(fileName);
	return TRUE;

}

void remmina_rdp_cliprdr_mt_server_format_data_request(RemminaProtocolWidget* gp, RemminaPluginRdpUiObject* ui)
{
	TRACE_CALL("remmina_rdp_cliprdr_mt_server_format_data_request");
	GtkClipboard* gtkClipboard;
	UINT8* inbuf = NULL;
	UINT8* outbuf = NULL;
	GdkPixbuf *image = NULL;
	int size = 0;
	gchar **uris, **urip;
	rfContext* rfi;
	rfClipboard *clipboard;
	FILEGROUPDESCRIPTORW* groupDsc;
	int i;

	printf("GIO: remmina_rdp_cliprdr_mt_server_format_data_request() called for ui->clipboard.format = %d\n", ui->clipboard.format);

	rfi = GET_PLUGIN_DATA(gp);
	if (!rfi)
		return;

	clipboard = rfi->clipboard;
	if (!clipboard)
		return;

	gtkClipboard = gtk_widget_get_clipboard(rfi->drawing_area, GDK_SELECTION_CLIPBOARD);
	if (!gtkClipboard)
		goto error;

	if (ui->clipboard.format == CF_LOCAL_FILEDESCRIPTOR) {
		/* Server is requesting us a file name, because we told him we have one
		 * when responding to remmina_rdp_cliprdr_mt_send_format_list.
		 *  */
		uris = gtk_clipboard_wait_for_uris(gtkClipboard);

		clear_file_array(clipboard);

		urip = uris;
		while(*urip) {
			if (remmina_rdp_cliprdr_is_valid_file_or_dir_uri(*urip)) {
				remmina_cliprdr_process_filename(clipboard, *urip);
			}
			urip++;
		}
		g_strfreev(uris);

		size = sizeof(FILEGROUPDESCRIPTORW) + (clipboard->nFiles - 1) * sizeof(FILEDESCRIPTORW);
		groupDsc = (FILEGROUPDESCRIPTORW*) malloc(size);
		printf("GIO: Allocated buffer of %u bytes at address %p\n", size, (void*)groupDsc);
		if (groupDsc) {
			groupDsc->cItems = clipboard->nFiles;
			for (i = 0; i < clipboard->nFiles; i++) {
				printf("GIO: nFiles=%d i=%d name=%s\n", (int)clipboard->nFiles, i, clipboard->file_names[i]);
				printf("GIO: clipboard->filedescriptor[%d] is at %p\n", i, (void *)clipboard->fileDescriptor[i]);
				if (clipboard->fileDescriptor[i])
					groupDsc->fgd[i] = *clipboard->fileDescriptor[i];
			}
			outbuf = (UINT8*)groupDsc;
		} else
			size = 0;

	} else {


		switch (ui->clipboard.format)
		{
			case CF_TEXT:
			case CF_UNICODETEXT:
			case CB_FORMAT_HTML:
			{
				inbuf = (UINT8*)gtk_clipboard_wait_for_text(gtkClipboard);
				break;
			}

			case CB_FORMAT_PNG:
			case CB_FORMAT_JPEG:
			case CF_DIB:
			case CF_DIBV5:
			{
				image = gtk_clipboard_wait_for_image(gtkClipboard);
				break;
			}
			case CF_LOCAL_FILEDESCRIPTOR:
			{
			}
		}

		if (inbuf != NULL || image != NULL)
		{
			switch (ui->clipboard.format)
			{
				case CF_TEXT:
				case CB_FORMAT_HTML:
				{
					size = strlen((char*)inbuf);
					outbuf = lf2crlf(inbuf, &size);
					break;
				}
				case CF_UNICODETEXT:
				{
					size = strlen((char*)inbuf);
					inbuf = lf2crlf(inbuf, &size);
					size = (ConvertToUnicode(CP_UTF8, 0, (CHAR*)inbuf, -1, (WCHAR**)&outbuf, 0) ) * sizeof(WCHAR);
					g_free(inbuf);
					break;
				}
				case CB_FORMAT_PNG:
				{
					gchar* data;
					gsize buffersize;
					gdk_pixbuf_save_to_buffer(image, &data, &buffersize, "png", NULL, NULL);
					outbuf = (UINT8*) malloc(buffersize);
					memcpy(outbuf, data, buffersize);
					size = buffersize;
					g_object_unref(image);
					break;
				}
				case CB_FORMAT_JPEG:
				{
					gchar* data;
					gsize buffersize;
					gdk_pixbuf_save_to_buffer(image, &data, &buffersize, "jpeg", NULL, NULL);
					outbuf = (UINT8*) malloc(buffersize);
					memcpy(outbuf, data, buffersize);
					size = buffersize;
					g_object_unref(image);
					break;
				}
				case CF_DIB:
				case CF_DIBV5:
				{
					gchar* data;
					gsize buffersize;
					gdk_pixbuf_save_to_buffer(image, &data, &buffersize, "bmp", NULL, NULL);
					size = buffersize - 14;
					outbuf = (UINT8*) malloc(size);
					memcpy(outbuf, data + 14, size);
					g_object_unref(image);
					break;
				}
			}
		}
	}

error:

	remmina_rdp_cliprdr_send_data_response(clipboard, outbuf, size);
}
void remmina_rdp_cliprdr_set_clipboard_content(RemminaProtocolWidget* gp, RemminaPluginRdpUiObject* ui)
{
	TRACE_CALL("remmina_rdp_cliprdr_set_clipboard_content");
	GtkClipboard* gtkClipboard;
	rfContext* rfi = GET_PLUGIN_DATA(gp);

	gtkClipboard = gtk_widget_get_clipboard(rfi->drawing_area, GDK_SELECTION_CLIPBOARD);
	if (ui->clipboard.format == CB_FORMAT_PNG || ui->clipboard.format == CF_DIB || ui->clipboard.format == CF_DIBV5 || ui->clipboard.format == CB_FORMAT_JPEG) {
		gtk_clipboard_set_image( gtkClipboard, ui->clipboard.data );
		g_object_unref(ui->clipboard.data);
	}
	else {
		gtk_clipboard_set_text( gtkClipboard, ui->clipboard.data, -1 );
		free(ui->clipboard.data);
	}

}

void remmina_rdp_cliprdr_set_clipboard_data(RemminaProtocolWidget* gp, RemminaPluginRdpUiObject* ui)
{
	TRACE_CALL("remmina_rdp_cliprdr_set_clipboard_data");
	GtkClipboard* gtkClipboard;
	GtkTargetEntry* targets;
	gint n_targets;
	rfContext* rfi;
	rfClipboard* clipboard;

	rfi = GET_PLUGIN_DATA(gp);
	if (!rfi)
		return;

	clipboard = rfi->clipboard;
	if (!clipboard)
		return;

	targets = gtk_target_table_new_from_list(ui->clipboard.targetlist, &n_targets);
	gtkClipboard = gtk_widget_get_clipboard(rfi->drawing_area, GDK_SELECTION_CLIPBOARD);
	if (gtkClipboard && targets)
	{
		gtk_clipboard_set_with_owner(gtkClipboard, targets, n_targets,
				(GtkClipboardGetFunc) remmina_rdp_cliprdr_request_data,
				(GtkClipboardClearFunc) remmina_rdp_cliprdr_empty_clipboard, G_OBJECT(gp));
		gtk_target_table_free(targets, n_targets);
	}
}

static void remmina_rdp_cliprdr_detach_owner(RemminaProtocolWidget* gp, RemminaPluginRdpUiObject* ui)
{
	/* When closing a rdp connection, we should check if gp is a clipboard owner.
	 * If it's an owner, detach it from the clipboard */
	TRACE_CALL("remmina_rdp_cliprdr_set_clipboard_data");
	rfContext* rfi = GET_PLUGIN_DATA(gp);
	GtkClipboard* gtkClipboard;

	gtkClipboard = gtk_widget_get_clipboard(rfi->drawing_area, GDK_SELECTION_CLIPBOARD);
	if (gtkClipboard && gtk_clipboard_get_owner(gtkClipboard) == (GObject*)gp) {
		gtk_clipboard_clear(gtkClipboard);
	}

}

void remmina_rdp_event_process_clipboard(RemminaProtocolWidget* gp, RemminaPluginRdpUiObject* ui)
{
	TRACE_CALL("remmina_rdp_event_process_clipboard");
	switch (ui->clipboard.type)
	{

		case REMMINA_RDP_UI_CLIPBOARD_FORMATLIST:
			remmina_rdp_cliprdr_mt_send_format_list(gp, ui);
			break;
		case REMMINA_RDP_UI_CLIPBOARD_MONITORREADY:
			remmina_rdp_cliprdr_mt_monitor_ready(gp, ui);
			break;

		case REMMINA_RDP_UI_CLIPBOARD_SERVER_FORMAT_DATA_REQUEST:
			remmina_rdp_cliprdr_mt_server_format_data_request(gp, ui);
			break;

		case REMMINA_RDP_UI_CLIPBOARD_SET_DATA:
			remmina_rdp_cliprdr_set_clipboard_data(gp, ui);
			break;

		case REMMINA_RDP_UI_CLIPBOARD_SET_CONTENT:
			remmina_rdp_cliprdr_set_clipboard_content(gp, ui);
			break;

		case REMMINA_RDP_UI_CLIPBOARD_DETACH_OWNER:
			remmina_rdp_cliprdr_detach_owner(gp, ui);
			break;

		case REMMINA_RDP_UI_CLIPBOARD_FILE_CONTENTS_REQUEST:
			remmina_rdp_cliprdr_mt_server_file_contents_request(gp, ui);
			break;

	}
}

gboolean remmina_rdp_clipboard_init(rfContext *rfi)
{
	TRACE_CALL("remmina_rdp_clipboard_init");
	rfi->clipboard = (rfClipboard*)malloc(sizeof(rfClipboard));
	if (!rfi->clipboard)
		return FALSE;
	rfi->clipboard->rfi = rfi;
	rfi->clipboard->lastFileContentsRequest = NULL;
	rfi->clipboard->nFiles = 0;
	rfi->clipboard->file_array_size = 0;
	rfi->clipboard->file_names = NULL;
	rfi->clipboard->fileDescriptor = NULL;
	return TRUE;
}

void remmina_rdp_clipboard_free(rfContext *rfi)
{
	TRACE_CALL("remmina_rdp_clipboard_free");

	if (!rfi->clipboard)
		return;

	// deinitialize rfi->clipboard

	if (rfi->clipboard->lastFileContentsRequest) {
		free(rfi->clipboard->lastFileContentsRequest);
		rfi->clipboard->lastFileContentsRequest = NULL;
	}

	clear_file_array(rfi->clipboard);

	free(rfi->clipboard);
	rfi->clipboard = NULL;
}


void remmina_rdp_cliprdr_init(rfContext* rfi, CliprdrClientContext* cliprdr)
{
	TRACE_CALL("remmina_rdp_cliprdr_init");

	printf("GIO: sizeof(LONG)=%u sizeof(WIN_POINTL)=%u sizeof(long)=%u sizeof(UINT)=%u\n", (unsigned)sizeof(LONG), (unsigned)sizeof(WIN_POINTL),
		(unsigned)sizeof(long),(unsigned)sizeof(UINT));

	if (!remmina_rdp_clipboard_init(rfi))
		return;

	cliprdr->custom = (void*)rfi->clipboard;
	rfi->clipboard->context = cliprdr;

	pthread_mutex_init(&rfi->clipboard->transfer_clip_mutex, NULL);
	pthread_cond_init(&rfi->clipboard->transfer_clip_cond,NULL);
	rfi->clipboard->srv_clip_data_wait = SCDW_NONE;

	cliprdr->MonitorReady = remmina_rdp_cliprdr_monitor_ready;
	cliprdr->ServerCapabilities = remmina_rdp_cliprdr_server_capabilities;
	cliprdr->ServerFormatList = remmina_rdp_cliprdr_server_format_list;
	cliprdr->ServerFormatListResponse = remmina_rdp_cliprdr_server_format_list_response;
	cliprdr->ServerFormatDataRequest = remmina_rdp_cliprdr_server_format_data_request;
	cliprdr->ServerFormatDataResponse = remmina_rdp_cliprdr_server_format_data_response;

	cliprdr->ServerFileContentsRequest = remmina_rdp_cliprdr_server_file_contents_request;
	cliprdr->ServerFileContentsResponse = remmina_rdp_cliprdr_server_file_contents_response;

}

