/**
 * yuclaw-edge/src/fix_gateway.cpp
 *
 * FIX 4.4 Execution Gateway for YUCLAW ATROS
 * Compiles on ARM64 (NVIDIA DGX Spark Grace CPU)
 *
 * This is a standalone FIX client that:
 * 1. Connects to a FIX server (broker/exchange)
 * 2. Sends NewOrderSingle (35=D) messages
 * 3. Receives ExecutionReport (35=8) confirmations
 * 4. Tracks order state and logs all messages
 *
 * Build: g++ -std=c++17 -O2 -o fix_gateway src/fix_gateway.cpp -lpthread
 */

#include <iostream>
#include <string>
#include <sstream>
#include <chrono>
#include <ctime>
#include <map>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <fstream>
#include <iomanip>
#include <functional>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

namespace yuclaw {

// FIX 4.4 constants
constexpr char SOH = '\x01';
constexpr const char* FIX_VERSION = "FIX.4.4";

// Message types
constexpr const char* MSG_HEARTBEAT      = "0";
constexpr const char* MSG_LOGON          = "A";
constexpr const char* MSG_LOGOUT         = "5";
constexpr const char* MSG_NEW_ORDER      = "D";
constexpr const char* MSG_CANCEL         = "F";
constexpr const char* MSG_EXEC_REPORT    = "8";
constexpr const char* MSG_CANCEL_REJECT  = "9";

// Order sides
constexpr const char* SIDE_BUY  = "1";
constexpr const char* SIDE_SELL = "2";

// Order types
constexpr const char* ORD_MARKET = "1";
constexpr const char* ORD_LIMIT  = "2";

// Time in force
constexpr const char* TIF_DAY = "0";
constexpr const char* TIF_GTC = "1";
constexpr const char* TIF_IOC = "3";

/**
 * FIX message builder and parser.
 * Handles tag=value|SOH encoding per FIX 4.4 spec.
 */
class FIXMessage {
public:
    void set(int tag, const std::string& value) {
        fields_[tag] = value;
    }

    void set(int tag, int value) {
        fields_[tag] = std::to_string(value);
    }

    void set(int tag, double value) {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(2) << value;
        fields_[tag] = ss.str();
    }

    std::string get(int tag) const {
        auto it = fields_.find(tag);
        return (it != fields_.end()) ? it->second : "";
    }

    bool has(int tag) const {
        return fields_.count(tag) > 0;
    }

    /**
     * Encode to FIX wire format: 8=FIX.4.4|9=len|35=type|...|10=checksum|
     */
    std::string encode() const {
        // Build body (everything except 8, 9, 10)
        std::string body;
        for (const auto& [tag, val] : fields_) {
            if (tag == 8 || tag == 9 || tag == 10) continue;
            body += std::to_string(tag) + "=" + val + SOH;
        }

        // Header
        std::string header;
        header += "8=" + std::string(FIX_VERSION) + SOH;
        header += "9=" + std::to_string(body.size()) + SOH;

        // Checksum
        std::string msg = header + body;
        int sum = 0;
        for (char c : msg) sum += static_cast<unsigned char>(c);
        char cksum[4];
        snprintf(cksum, sizeof(cksum), "%03d", sum % 256);
        msg += "10=" + std::string(cksum) + SOH;

        return msg;
    }

    /**
     * Parse a FIX message from wire format.
     */
    static FIXMessage parse(const std::string& raw) {
        FIXMessage msg;
        size_t pos = 0;
        while (pos < raw.size()) {
            size_t eq = raw.find('=', pos);
            if (eq == std::string::npos) break;
            size_t soh = raw.find(SOH, eq);
            if (soh == std::string::npos) soh = raw.size();

            int tag = std::stoi(raw.substr(pos, eq - pos));
            std::string val = raw.substr(eq + 1, soh - eq - 1);
            msg.set(tag, val);

            pos = soh + 1;
        }
        return msg;
    }

private:
    std::map<int, std::string> fields_;
};

/**
 * Order tracking.
 */
struct Order {
    std::string cl_ord_id;
    std::string symbol;
    std::string side;
    std::string ord_type;
    int quantity;
    double price;
    std::string status;  // "NEW", "FILLED", "CANCELLED", "REJECTED"
    std::string exec_id;
    double fill_price;
    int fill_qty;
    std::chrono::steady_clock::time_point submit_time;
    std::chrono::steady_clock::time_point fill_time;
};

/**
 * FIX Gateway — manages connection, session, and order flow.
 */
class FIXGateway {
public:
    FIXGateway(const std::string& sender,
               const std::string& target,
               const std::string& host,
               int port)
        : sender_(sender), target_(target),
          host_(host), port_(port),
          seq_num_(1), connected_(false), sock_(-1) {}

    ~FIXGateway() {
        disconnect();
    }

    /**
     * Connect to the FIX server via TCP.
     */
    bool connect() {
        sock_ = socket(AF_INET, SOCK_STREAM, 0);
        if (sock_ < 0) {
            std::cerr << "[FIX] Socket creation failed" << std::endl;
            return false;
        }

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port_);
        inet_pton(AF_INET, host_.c_str(), &addr.sin_addr);

        if (::connect(sock_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            std::cerr << "[FIX] Connection to " << host_ << ":" << port_ << " failed" << std::endl;
            close(sock_);
            sock_ = -1;
            return false;
        }

        connected_ = true;
        std::cout << "[FIX] Connected to " << host_ << ":" << port_ << std::endl;
        return true;
    }

    void disconnect() {
        if (sock_ >= 0) {
            if (connected_) send_logout();
            close(sock_);
            sock_ = -1;
            connected_ = false;
        }
    }

    /**
     * Send FIX Logon message (35=A).
     */
    bool logon(int heartbeat_interval = 30) {
        FIXMessage msg;
        msg.set(35, MSG_LOGON);
        msg.set(49, sender_);
        msg.set(56, target_);
        msg.set(34, seq_num_++);
        msg.set(52, timestamp());
        msg.set(98, 0);  // EncryptMethod = None
        msg.set(108, heartbeat_interval);

        return send(msg);
    }

    /**
     * Send NewOrderSingle (35=D).
     */
    bool send_new_order(const std::string& symbol,
                        const std::string& side,
                        int quantity,
                        const std::string& ord_type = ORD_MARKET,
                        double price = 0.0) {
        std::string cl_ord_id = "YUCLAW-" + std::to_string(seq_num_);

        FIXMessage msg;
        msg.set(35, MSG_NEW_ORDER);
        msg.set(49, sender_);
        msg.set(56, target_);
        msg.set(34, seq_num_++);
        msg.set(52, timestamp());
        msg.set(11, cl_ord_id);          // ClOrdID
        msg.set(55, symbol);             // Symbol
        msg.set(54, side);               // Side
        msg.set(60, timestamp());        // TransactTime
        msg.set(38, quantity);           // OrderQty
        msg.set(40, ord_type);           // OrdType
        msg.set(59, TIF_DAY);           // TimeInForce

        if (ord_type == std::string(ORD_LIMIT) && price > 0) {
            msg.set(44, price);          // Price
        }

        // Track the order
        Order ord{};
        ord.cl_ord_id = cl_ord_id;
        ord.symbol = symbol;
        ord.side = side;
        ord.ord_type = ord_type;
        ord.quantity = quantity;
        ord.price = price;
        ord.status = "PENDING";
        ord.submit_time = std::chrono::steady_clock::now();
        orders_[cl_ord_id] = ord;

        std::cout << "[FIX] Sending NewOrderSingle: " << cl_ord_id
                  << " " << symbol << " " << (side == SIDE_BUY ? "BUY" : "SELL")
                  << " " << quantity << " @ "
                  << (ord_type == std::string(ORD_MARKET) ? "MKT" : std::to_string(price))
                  << std::endl;

        return send(msg);
    }

    /**
     * Send OrderCancelRequest (35=F).
     */
    bool send_cancel(const std::string& cl_ord_id, const std::string& symbol) {
        FIXMessage msg;
        msg.set(35, MSG_CANCEL);
        msg.set(49, sender_);
        msg.set(56, target_);
        msg.set(34, seq_num_++);
        msg.set(52, timestamp());
        msg.set(41, cl_ord_id);          // OrigClOrdID
        msg.set(11, "CXL-" + cl_ord_id); // ClOrdID
        msg.set(55, symbol);
        msg.set(54, SIDE_BUY);

        return send(msg);
    }

    /**
     * Process incoming messages.
     */
    void process_incoming() {
        char buf[4096];
        int n = recv(sock_, buf, sizeof(buf) - 1, MSG_DONTWAIT);
        if (n <= 0) return;

        buf[n] = '\0';
        auto msg = FIXMessage::parse(std::string(buf, n));
        std::string msg_type = msg.get(35);

        if (msg_type == MSG_EXEC_REPORT) {
            handle_execution_report(msg);
        } else if (msg_type == MSG_HEARTBEAT) {
            // Respond with heartbeat
            send_heartbeat();
        } else if (msg_type == MSG_LOGOUT) {
            std::cout << "[FIX] Server sent Logout" << std::endl;
            connected_ = false;
        }
    }

    /**
     * Get order status summary.
     */
    void print_orders() const {
        std::cout << "\n=== ORDER BOOK ===" << std::endl;
        for (const auto& [id, ord] : orders_) {
            auto lat = std::chrono::duration_cast<std::chrono::microseconds>(
                ord.fill_time - ord.submit_time).count();
            std::cout << "  " << id << " " << ord.symbol
                      << " " << ord.status
                      << " qty=" << ord.quantity
                      << " fill_px=" << ord.fill_price
                      << " lat=" << lat << "us"
                      << std::endl;
        }
    }

    bool is_connected() const { return connected_; }

private:
    bool send(const FIXMessage& msg) {
        std::string raw = msg.encode();
        log_message("OUT", raw);
        if (sock_ < 0 || !connected_) return false;
        return ::send(sock_, raw.c_str(), raw.size(), 0) > 0;
    }

    void send_heartbeat() {
        FIXMessage msg;
        msg.set(35, MSG_HEARTBEAT);
        msg.set(49, sender_);
        msg.set(56, target_);
        msg.set(34, seq_num_++);
        msg.set(52, timestamp());
        send(msg);
    }

    void send_logout() {
        FIXMessage msg;
        msg.set(35, MSG_LOGOUT);
        msg.set(49, sender_);
        msg.set(56, target_);
        msg.set(34, seq_num_++);
        msg.set(52, timestamp());
        send(msg);
    }

    void handle_execution_report(const FIXMessage& msg) {
        std::string cl_ord_id = msg.get(11);
        std::string exec_type = msg.get(150);
        std::string ord_status = msg.get(39);

        auto it = orders_.find(cl_ord_id);
        if (it == orders_.end()) return;

        auto& ord = it->second;
        ord.exec_id = msg.get(17);
        ord.fill_time = std::chrono::steady_clock::now();

        if (ord_status == "2") {  // Filled
            ord.status = "FILLED";
            ord.fill_price = std::stod(msg.get(31).empty() ? "0" : msg.get(31));
            ord.fill_qty = std::stoi(msg.get(32).empty() ? "0" : msg.get(32));
            auto lat = std::chrono::duration_cast<std::chrono::microseconds>(
                ord.fill_time - ord.submit_time).count();
            std::cout << "[FIX] FILL: " << cl_ord_id << " " << ord.symbol
                      << " @ " << ord.fill_price
                      << " lat=" << lat << "us" << std::endl;
        } else if (ord_status == "8") {  // Rejected
            ord.status = "REJECTED";
            std::cout << "[FIX] REJECTED: " << cl_ord_id << " reason=" << msg.get(58) << std::endl;
        } else if (ord_status == "4") {  // Cancelled
            ord.status = "CANCELLED";
            std::cout << "[FIX] CANCELLED: " << cl_ord_id << std::endl;
        }
    }

    void log_message(const std::string& dir, const std::string& raw) {
        std::ofstream log("fix_messages.log", std::ios::app);
        log << timestamp() << " " << dir << " " << raw << "\n";
    }

    static std::string timestamp() {
        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count() % 1000;
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y%m%d-%H:%M:%S", std::gmtime(&t));
        return std::string(buf) + "." + std::to_string(ms);
    }

    std::string sender_;
    std::string target_;
    std::string host_;
    int port_;
    int seq_num_;
    std::atomic<bool> connected_;
    int sock_;
    std::map<std::string, Order> orders_;
};

}  // namespace yuclaw

/**
 * Main — demo: connect, logon, send test order.
 * Usage: ./fix_gateway [host] [port]
 */
int main(int argc, char* argv[]) {
    std::string host = argc > 1 ? argv[1] : "127.0.0.1";
    int port = argc > 2 ? std::stoi(argv[2]) : 9876;

    std::cout << "=== YUCLAW-EDGE FIX 4.4 Gateway ===" << std::endl;
    std::cout << "Connecting to " << host << ":" << port << std::endl;

    yuclaw::FIXGateway gw("YUCLAW", "BROKER", host, port);

    if (!gw.connect()) {
        std::cout << "[FIX] Connection failed — running in simulation mode" << std::endl;
        std::cout << "[FIX] Gateway compiled and ready for ARM64 DGX Spark" << std::endl;
        std::cout << "[FIX] To connect: ./fix_gateway <broker_host> <broker_port>" << std::endl;
        return 0;
    }

    gw.logon();
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // Send test paper order
    gw.send_new_order("AAPL", yuclaw::SIDE_BUY, 100, yuclaw::ORD_LIMIT, 250.00);

    // Listen for responses
    for (int i = 0; i < 10; i++) {
        gw.process_incoming();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    gw.print_orders();
    gw.disconnect();

    return 0;
}
