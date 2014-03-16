#include "QtWebThread.h"
#include "QtWebRequest.h"

#include <QtCore/QDebug>
#include <QtCore/QFile>

class QtWebThreadPrivate
{
    public:
        qintptr socketHandle;
        bool isUsingSecureConnections;

        QTcpSocket * socket = Q_NULLPTR;

        QByteArray data;
        qint64 bytesWritten;

        QByteArray requestData;
        QByteArray postBoundary;

        QtWebRequest * request;
};

QtWebThread::QtWebThread(QObject *parent) :
    QThread(parent),
    d_ptr(new QtWebThreadPrivate)
{
    QObject::connect( this, &QtWebThread::started,
                      this, &QtWebThread::startHandlingConnection,
                      // This is needed so the slots is still in the same thread
                      Qt::DirectConnection
                      );

    setTerminationEnabled(true);
}

void QtWebThread::setSocketHandle(qintptr handle)
{
    Q_D(QtWebThread);

    d->socketHandle = handle;
}

void QtWebThread::setSecureSocket(bool isSecure)
{
    Q_D(QtWebThread);

    d->isUsingSecureConnections = isSecure;
}

void QtWebThread::startHandlingConnection()
{
    Q_D(QtWebThread);

    qDebug() << currentThread() << "startHandlingConnection";

    d->socket = new QTcpSocket;
    d->socket->setSocketDescriptor(d->socketHandle);

    QObject::connect( d->socket, &QTcpSocket::readyRead,
                      this, &QtWebThread::readyToRead,
                      Qt::DirectConnection
                      );

    QObject::connect( this, &QtWebThread::everythingParsed,
                      this, &QtWebThread::readyToWrite,
                      Qt::DirectConnection
                      );
}

void QtWebThread::readyToRead()
{
    Q_D(QtWebThread);

    qDebug() << currentThread() << "readyToRead" ;


    QObject::disconnect( d->socket, &QTcpSocket::readyRead,
                      this, &QtWebThread::readyToRead
                      );

    QObject::connect( d->socket, &QTcpSocket::readyRead,
                      this, &QtWebThread::readyToReadPostData,
                      Qt::DirectConnection
                      );

    QByteArray data = d->socket->readAll();

    //qDebug() << data;

    QByteArray methodString = data.left(3);

    d->request = new QtWebRequest;
    QByteArray CRLF("\r\n");

    // Method
    QtWebRequest::RequestMethod method;

    if ( methodString == "GET" ) {
        method = QtWebRequest::RequestMethod::Get;
    }
    else if ( methodString == "POS" ) {
        method = QtWebRequest::RequestMethod::Post;
    }
    else if ( methodString == "HEA" ) {
        method = QtWebRequest::RequestMethod::Head;
    }
    else {
        method = QtWebRequest::RequestMethod::Unsupported;
    }

    d->request->setMethod(method);

    // Request path
    int indexOfRequestPathStart = data.indexOf("/");
    int indexOfRequestPathEnd = data.indexOf(" ", indexOfRequestPathStart);
    QByteArray requestPath = data.mid(indexOfRequestPathStart, indexOfRequestPathEnd - indexOfRequestPathStart);

    d->request->setRequestPath(requestPath);

    // Http version
    int indexOfBreak = data.indexOf(CRLF);
    QByteArray httpVersionString = data.mid(indexOfRequestPathEnd + 1, indexOfBreak - indexOfRequestPathEnd - 1);
    QtWebRequest::HttpVersion httpVersion = QtWebRequest::HttpVersion::Unknown;

    if ( httpVersionString == "HTTP/1.1" ) {
        httpVersion = QtWebRequest::HttpVersion::v1_1;
    }
    if ( httpVersionString == "HTTP/1.0" ) {
        httpVersion = QtWebRequest::HttpVersion::v1_0;
    }

    d->request->setHttpVersion(httpVersion);

    data = data.remove(0, indexOfBreak + CRLF.length());

    // Headers
    QHash<QByteArray, QByteArray> headers;

    Q_FOREVER {
        indexOfBreak = data.indexOf(CRLF);
        QByteArray headerLine = data.left(indexOfBreak);

        QByteArray key;
        QByteArray value;

        QList<QByteArray> splittedLine = headerLine.split(':');

        key = splittedLine.first();

        if ( splittedLine.length() >= 2 ) {

            value = splittedLine.at(1);
            value = value.right(value.length() - 1);

            for (int i = 2; i < splittedLine.length(); ++i) {
                value += ":" + splittedLine.at(i);
            }
        }
        else {
            value = "";
        }

        headers.insert(key, value);

        data = data.remove(0, indexOfBreak + CRLF.length());

        if ( data.startsWith(CRLF) ) {
            data = data.remove(0, CRLF.length());

            break;
        }
    }

    d->request->setHeaders(headers);

    QByteArray contentType = headers.value("Content-Type");
    QByteArray multiPart("multipart/form-data; ");
    int indexOfStart = contentType.indexOf(multiPart);

    if ( indexOfStart == 0 ) {
        d->requestData = data;
        d->postBoundary = contentType.mid(multiPart.length());
    }
    else {
        Q_EMIT everythingParsed();
    }
}

void QtWebThread::readyToReadPostData()
{
    Q_D(QtWebThread);

    qDebug() << QThread::currentThread() << "readyToReadPostData";

    QByteArray newData = d->socket->readAll();
    d->requestData += newData;

    int indexOfEnd = newData.indexOf("--\r\n");

    if ( indexOfEnd != -1 ) {

        parsePostData();

        Q_EMIT everythingParsed();
    }
}

void QtWebThread::parsePostData()
{
    Q_D(QtWebThread);

    QByteArray boundarySearchString("boundary=");
    QByteArray boundaryPart = d->postBoundary;
    auto indexOfBoundary = boundaryPart.indexOf(boundarySearchString);
    if ( indexOfBoundary == -1 ) return;
    QByteArray boundary = boundaryPart.remove(indexOfBoundary, boundarySearchString.length());

    QByteArray start_end("--");
    QByteArray line_break("\r\n");
    QByteArray contentDisposition("Content-Disposition: ");
    QByteArray contentType("Content-Type: ");
    QByteArray name(" name=");
    QByteArray fileName(" filename=");

    QByteArray bodyData = d->requestData;

    Q_FOREVER {
        auto indexOfStart = bodyData.indexOf(start_end);
        if ( indexOfStart == -1 ) break;
        bodyData = bodyData.remove(indexOfStart, start_end.length());

        auto indexOfBoundary = bodyData.indexOf(boundary);
        if ( indexOfBoundary == -1 ) break;
        bodyData = bodyData.remove(indexOfBoundary, boundary.length());

        auto indexOfBreak = bodyData.indexOf(line_break);
        if ( indexOfBreak == -1 ) break;
        bodyData = bodyData.remove(indexOfBreak, line_break.length());

        auto indexOfContentDisposition = bodyData.indexOf(contentDisposition);
        if ( indexOfContentDisposition == -1 ) break;
        bodyData = bodyData.remove(indexOfContentDisposition, contentDisposition.length());

        indexOfBreak = bodyData.indexOf(line_break);
        QByteArray contentDispositionLine = bodyData.left(indexOfBreak);
        QList<QByteArray> splittedDisposition = contentDispositionLine.split(';');
        bodyData = bodyData.remove(0, indexOfBreak + line_break.length());

        QByteArray namePart = splittedDisposition.at(1);

        auto indexOfName = namePart.indexOf(name);
        if ( indexOfName == -1 ) break;
        namePart = namePart.remove(indexOfName, name.length());

        QByteArray nameValue = namePart.left(namePart.length() -1);
        nameValue = nameValue.right(nameValue.length() -1);

        if ( splittedDisposition.size() == 2 ) {

            auto indexOfBreak = bodyData.indexOf(line_break);
            if ( indexOfBreak == -1 ) break;
            bodyData = bodyData.remove(indexOfBreak, line_break.length());

            indexOfBreak = bodyData.indexOf(line_break);
            if ( indexOfBreak == -1 ) break;
            QByteArray value = bodyData.mid(0, indexOfBreak);
            bodyData = bodyData.remove(0, indexOfBreak + line_break.length());

            d->request->insertPostValue(nameValue, value);
        }
        else {
            QByteArray fileNamePart = splittedDisposition.at(2);

            auto indexOfFileName = fileNamePart.indexOf(fileName);
            if ( indexOfFileName == -1 ) break;
            fileNamePart = fileNamePart.remove(indexOfFileName, fileName.length());

            QByteArray fileNameValue = fileNamePart.left(fileNamePart.length() -1);
            fileNameValue = fileNameValue.right(fileNameValue.length() -1);

            auto indexOfContentType = bodyData.indexOf(contentType);
            if ( indexOfContentType == -1 ) break;
            bodyData = bodyData.remove(indexOfContentType, contentType.length());

            indexOfBreak = bodyData.indexOf(line_break);
            if ( indexOfBreak == -1 ) break;
            QByteArray mimeType = bodyData.left(indexOfBreak);
            bodyData = bodyData.remove(0, indexOfBreak + line_break.length());

            indexOfBreak = bodyData.indexOf(line_break);
            if ( indexOfBreak == -1 ) break;
            bodyData = bodyData.remove(indexOfBreak, line_break.length());

            //
            QFile file(fileNameValue);
            if ( file.open(QIODevice::WriteOnly | QIODevice::Unbuffered) ) {

                const int space = 10000;

                QByteArray data;
                data.reserve(space);
                indexOfBreak = -1;

                qDebug() << bodyData.indexOf("--\r\n");

                while ( indexOfBreak == -1 ) {
                    data = bodyData.left(space);

                    indexOfBreak = data.indexOf("--\r\n");

                    if ( indexOfBreak == -1 ) {
                        bodyData = bodyData.remove(0, space);
                        file.write(data);
                    }
                    else {
                        data = bodyData.left(indexOfBreak);
                        bodyData = bodyData.remove(0, indexOfBreak);

                        file.write(data);
                    }
                }

                file.close();
            }
        }
    }
}

void QtWebThread::readyToWrite()
{
    Q_D(QtWebThread);

    qDebug() << currentThread() << "readyToWrite";

    QObject::connect( d->socket, &QTcpSocket::bytesWritten,
                      this, &QtWebThread::finishConnection,
                      Qt::DirectConnection
                      );

    const char data[] = "HTTP/1.1 202 OK\r\n";
    d->data.append(data);

    QByteArray body("<html><body><form action=\".\" method=\"post\" enctype=\"multipart/form-data\">"
                    "<input name=\"file_dd\" type=\"file\" /><input type=\"submit\" />"
                    "</form></body></html>");

    QByteArray header = "Content-Length; " + QByteArray::number(body.length()) + "\r\n";

    d->data.append(header);
    d->data.append("\r\n");
    d->data.append(body);
    d->data.append("\r\n");

    d->bytesWritten = 0;

    d->socket->write(d->data);
}

void QtWebThread::finishConnection(qint64 bytes)
{
    Q_D(QtWebThread);

    qDebug() << currentThread() << "finishConnection";

    d->bytesWritten += bytes;

    if ( d->bytesWritten == d->data.size() ) {
        d->socket->close();

        Q_EMIT finishedThisRequest();
    }
}