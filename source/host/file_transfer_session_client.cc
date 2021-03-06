//
// PROJECT:         Aspia Remote Desktop
// FILE:            host/file_transfer_session_client.cc
// LICENSE:         Mozilla Public License Version 2.0
// PROGRAMMERS:     Dmitry Chapyshev (dmitry@aspia.ru)
//

#include "host/file_transfer_session_client.h"
#include "base/files/file_helpers.h"
#include "base/strings/unicode.h"
#include "protocol/message_serialization.h"
#include "protocol/filesystem.h"
#include "proto/auth_session.pb.h"

namespace aspia {

namespace fs = std::experimental::filesystem;

void FileTransferSessionClient::Run(const std::wstring& input_channel_name,
                                    const std::wstring& output_channel_name)
{
    status_dialog_ = std::make_unique<UiFileStatusDialog>();

    ipc_channel_ = PipeChannel::CreateClient(input_channel_name,
                                             output_channel_name);
    if (ipc_channel_)
    {
        if (ipc_channel_->Connect(GetCurrentProcessId(), this))
        {
            status_dialog_->WaitForClose();
        }

        ipc_channel_.reset();
    }

    status_dialog_.reset();
}

void FileTransferSessionClient::OnPipeChannelConnect(uint32_t user_data)
{
    // The server sends the session type in user_data.
    proto::SessionType session_type = static_cast<proto::SessionType>(user_data);

    if (session_type != proto::SessionType::SESSION_TYPE_FILE_TRANSFER)
    {
        LOG(FATAL) << "Invalid session type passed: " << session_type;
        return;
    }

    status_dialog_->SetSessionStartedStatus();
}

void FileTransferSessionClient::OnPipeChannelDisconnect()
{
    status_dialog_->SetSessionTerminatedStatus();
}

void FileTransferSessionClient::OnPipeChannelMessage(const IOBuffer& buffer)
{
    proto::file_transfer::ClientToHost message;

    if (!ParseMessage(buffer, message))
    {
        ipc_channel_->Close();
        return;
    }

    switch (message.type())
    {
        case proto::RequestType::REQUEST_TYPE_DRIVE_LIST:
            ReadDriveListRequest();
            break;

        case proto::RequestType::REQUEST_TYPE_FILE_LIST:
            ReadFileListRequest(message.file_list_request());
            break;

        case proto::RequestType::REQUEST_TYPE_DIRECTORY_SIZE:
            ReadDirectorySizeRequest(message.directory_size_request());
            break;

        case proto::RequestType::REQUEST_TYPE_CREATE_DIRECTORY:
            ReadCreateDirectoryRequest(message.create_directory_request());
            break;

        case proto::RequestType::REQUEST_TYPE_RENAME:
            ReadRenameRequest(message.rename_request());
            break;

        case proto::RequestType::REQUEST_TYPE_REMOVE:
            ReadRemoveRequest(message.remove_request());
            break;

        case proto::RequestType::REQUEST_TYPE_FILE_UPLOAD:
            ReadFileUploadRequest(message.file_upload_request());
            break;

        case proto::RequestType::REQUEST_TYPE_FILE_UPLOAD_DATA:
        {
            if (!ReadFileUploadDataRequest(message.file_packet()))
                ipc_channel_->Close();
        }
        break;

        case proto::RequestType::REQUEST_TYPE_FILE_DOWNLOAD:
            ReadFileDownloadRequest(message.file_download_request());
            break;

        case proto::RequestType::REQUEST_TYPE_FILE_DOWNLOAD_DATA:
        {
            if (!ReadFileDownloadDataRequest())
                ipc_channel_->Close();
        }
        break;

        default:
            LOG(ERROR) << "Unknown message from client: " << message.type();
            ipc_channel_->Close();
            break;
    }
}

void FileTransferSessionClient::SendReply(
    const proto::file_transfer::HostToClient& reply)
{
    IOBuffer buffer(SerializeMessage<IOBuffer>(reply));
    std::lock_guard<std::mutex> lock(outgoing_lock_);
    ipc_channel_->Send(buffer);
}

void FileTransferSessionClient::ReadDriveListRequest()
{
    proto::file_transfer::HostToClient reply;
    reply.set_type(proto::RequestType::REQUEST_TYPE_DRIVE_LIST);
    reply.set_status(ExecuteDriveListRequest(reply.mutable_drive_list()));

    status_dialog_->SetDriveListRequestStatus(reply.status());
    SendReply(reply);
}

void FileTransferSessionClient::ReadFileListRequest(
    const proto::FileListRequest& request)
{
    proto::file_transfer::HostToClient reply;

    FilePath path = fs::u8path(request.path());

    reply.set_type(proto::RequestType::REQUEST_TYPE_FILE_LIST);
    reply.set_status(ExecuteFileListRequest(path, reply.mutable_file_list()));

    status_dialog_->SetFileListRequestStatus(path, reply.status());
    SendReply(reply);
}

void FileTransferSessionClient::ReadCreateDirectoryRequest(
    const proto::CreateDirectoryRequest& request)
{
    proto::file_transfer::HostToClient reply;

    FilePath path = fs::u8path(request.path());

    reply.set_type(proto::RequestType::REQUEST_TYPE_CREATE_DIRECTORY);
    reply.set_status(ExecuteCreateDirectoryRequest(path));

    status_dialog_->SetCreateDirectoryRequestStatus(path, reply.status());
    SendReply(reply);
}

void FileTransferSessionClient::ReadDirectorySizeRequest(
    const proto::DirectorySizeRequest& request)
{
    proto::file_transfer::HostToClient reply;

    FilePath path = fs::u8path(request.path());

    reply.set_type(proto::RequestType::REQUEST_TYPE_DIRECTORY_SIZE);

    uint64_t directory_size = 0;

    reply.set_status(ExecuteDirectorySizeRequest(path, directory_size));
    reply.mutable_directory_size()->set_size(directory_size);

    SendReply(reply);
}

void FileTransferSessionClient::ReadRenameRequest(
    const proto::RenameRequest& request)
{
    proto::file_transfer::HostToClient reply;

    FilePath old_name = fs::u8path(request.old_name());
    FilePath new_name = fs::u8path(request.new_name());

    reply.set_type(proto::RequestType::REQUEST_TYPE_RENAME);
    reply.set_status(ExecuteRenameRequest(old_name, new_name));

    status_dialog_->SetRenameRequestStatus(old_name, new_name, reply.status());
    SendReply(reply);
}

void FileTransferSessionClient::ReadRemoveRequest(
    const proto::RemoveRequest& request)
{
    proto::file_transfer::HostToClient reply;

    FilePath path = fs::u8path(request.path());

    reply.set_type(proto::RequestType::REQUEST_TYPE_REMOVE);
    reply.set_status(ExecuteRemoveRequest(path));

    status_dialog_->SetRemoveRequestStatus(path, reply.status());
    SendReply(reply);
}

void FileTransferSessionClient::ReadFileUploadRequest(
    const proto::FileUploadRequest& request)
{
    proto::file_transfer::HostToClient reply;
    reply.set_type(proto::RequestType::REQUEST_TYPE_FILE_UPLOAD);

    FilePath file_path = fs::u8path(request.file_path());

    if (!IsValidPathName(file_path))
    {
        reply.set_status(proto::RequestStatus::REQUEST_STATUS_INVALID_PATH_NAME);
    }
    else
    {
        file_depacketizer_ = FileDepacketizer::Create(file_path);
        if (!file_depacketizer_)
        {
            reply.set_status(proto::RequestStatus::REQUEST_STATUS_FILE_CREATE_ERROR);
        }
    }

    SendReply(reply);
}

bool FileTransferSessionClient::ReadFileUploadDataRequest(
    const proto::FilePacket& file_packet)
{
    if (!file_depacketizer_)
    {
        LOG(ERROR) << "Unexpected upload data request";
        return false;
    }

    proto::file_transfer::HostToClient reply;
    reply.set_type(proto::RequestType::REQUEST_TYPE_FILE_UPLOAD_DATA);

    if (!file_depacketizer_->ReadNextPacket(file_packet))
    {
        reply.set_status(proto::RequestStatus::REQUEST_STATUS_FILE_WRITE_ERROR);
    }

    if (file_packet.flags() & proto::FilePacket::LAST_PACKET)
    {
        file_depacketizer_.reset();
    }

    SendReply(reply);
    return true;
}

void FileTransferSessionClient::ReadFileDownloadRequest(
    const proto::FileDownloadRequest& request)
{
    proto::file_transfer::HostToClient reply;
    reply.set_type(proto::RequestType::REQUEST_TYPE_FILE_DOWNLOAD);

    FilePath file_path = fs::u8path(request.file_path());

    if (!IsValidPathName(file_path))
    {
        reply.set_status(proto::RequestStatus::REQUEST_STATUS_INVALID_PATH_NAME);
    }
    else
    {
        file_packetizer_ = FilePacketizer::Create(file_path);
        if (!file_packetizer_)
        {
            reply.set_status(proto::RequestStatus::REQUEST_STATUS_FILE_OPEN_ERROR);
        }
    }

    SendReply(reply);
}

bool FileTransferSessionClient::ReadFileDownloadDataRequest()
{
    if (!file_packetizer_)
    {
        LOG(ERROR) << "Unexpected download data request";
        return false;
    }

    proto::file_transfer::HostToClient reply;
    reply.set_type(proto::RequestType::REQUEST_TYPE_FILE_DOWNLOAD_DATA);

    std::unique_ptr<proto::FilePacket> packet =
        file_packetizer_->CreateNextPacket();

    if (!packet)
    {
        reply.set_status(proto::RequestStatus::REQUEST_STATUS_FILE_READ_ERROR);
    }
    else
    {
        if (packet->flags() & proto::FilePacket::LAST_PACKET)
        {
            file_packetizer_.reset();
        }

        reply.set_allocated_file_packet(packet.release());
    }

    SendReply(reply);
    return true;
}

} // namespace aspia
