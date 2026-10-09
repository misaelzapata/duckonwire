#include "duckonwire/scram.hpp"

#include <openssl/crypto.h>

#include <csignal>
#include <iostream>
#include <stdexcept>
#include <string>
#include <termios.h>
#include <unistd.h>

namespace {
volatile std::sig_atomic_t interrupted = 0;
void InterruptInput(int) { interrupted = 1; }
class HiddenInput {
public:
    HiddenInput() {
        if (!isatty(STDIN_FILENO)) return;
        if (tcgetattr(STDIN_FILENO, &original) != 0)
            throw std::runtime_error("Cannot protect password input");
        struct sigaction action {};
        action.sa_handler = InterruptInput;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGINT, &action, &previous_interrupt) != 0)
            throw std::runtime_error("Cannot protect password input");
        if (sigaction(SIGTERM, &action, &previous_termination) != 0) {
            sigaction(SIGINT, &previous_interrupt, nullptr);
            throw std::runtime_error("Cannot protect password input");
        }
        auto hidden = original;
        hidden.c_lflag &= ~ECHO;
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &hidden) != 0) {
            sigaction(SIGINT, &previous_interrupt, nullptr);
            sigaction(SIGTERM, &previous_termination, nullptr);
            throw std::runtime_error("Cannot protect password input");
        }
        active = true;
        std::cerr << "Password (32+ printable ASCII characters): " << std::flush;
    }
    ~HiddenInput() {
        if (!active) return;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &original);
        std::cerr << '\n';
        sigaction(SIGINT, &previous_interrupt, nullptr);
        sigaction(SIGTERM, &previous_termination, nullptr);
    }
private:
    bool active = false;
    termios original {};
    struct sigaction previous_interrupt {}, previous_termination {};
};
}

int main(int argc, char **argv) {
    if (argc == 2 && std::string(argv[1]) == "--help") {
        std::cout << "duckonwire-password\nReads a password from stdin (hidden on a terminal) and prints a SCRAM verifier.\n";
        return 0;
    }
    if (argc != 1) {
        std::cerr << "Passwords must be supplied on stdin, never as command arguments\n";
        return 1;
    }
    try {
        std::string password;
        {
            HiddenInput protect;
            for (int next; (next = std::cin.get()) != EOF && next != '\n';) {
                if (interrupted) throw std::runtime_error("Password input interrupted");
                if (password.size() == 1024) throw std::runtime_error("Invalid password length");
                password.push_back(static_cast<char>(next));
            }
            if (interrupted) throw std::runtime_error("Password input interrupted");
        }
        if (password.size() < 32 || password.size() > 1024)
            throw std::runtime_error("Password must contain 32 to 1024 printable ASCII characters");
        for (unsigned char byte : password)
            if (byte < 32 || byte > 126) throw std::runtime_error("Password must contain printable ASCII characters");
        auto verifier = duckonwire::MakeScramVerifier(password);
        OPENSSL_cleanse(password.data(), password.size());
        std::cout << verifier << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
